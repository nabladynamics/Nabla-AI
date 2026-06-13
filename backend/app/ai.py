"""AI experiment-definition co-pilot (Anthropic API).

Claude acts as the Nabla experiment co-pilot: it reads the geometry report,
identifies the object, asks the disambiguating questions from the spec
(experiment type, inlet velocity / Reynolds number, target outputs, steady vs
transient), and — only when enough is known — emits a structured experiment
card by calling a strict-schema tool.

Safety rule: the model may only PROPOSE. The card is schema-constrained by the
API (``strict: true``), validated again with Pydantic here, and mapped onto
:class:`~app.models.CaseConfig` through a typed whitelist. Free text can never
reach the solver configuration.
"""

from __future__ import annotations

import json
import re
from typing import Any, Literal, cast

import anthropic
from anthropic.types import (
    Message,
    MessageParam,
    TextBlock,
    TextBlockParam,
    ToolUnionParam,
    ToolUseBlock,
)
from pydantic import BaseModel, ConfigDict, Field, ValidationError

from app.models import CaseConfig

TargetOutput = Literal[
    "cd",
    "cl",
    "strouhal",
    "reattachment_length",
    "velocity_profiles",
    "pressure_distribution",
]


class ExperimentCard(BaseModel):
    """Structured experiment definition the solver config is built from."""

    model_config = ConfigDict(extra="forbid")

    title: str = Field(min_length=1, max_length=120)
    object_identification: str = Field(min_length=1, max_length=300)
    case_type: Literal["wall-mounted-cube", "channel", "lid-cavity"]
    reynolds_number: float = Field(gt=0, le=1e7)
    inlet_velocity: float = Field(gt=0, le=1e4)
    inlet_profile: Literal["uniform", "parabolic"]
    steady: bool
    target_outputs: list[TargetOutput] = Field(min_length=1)
    resolution: int = Field(ge=2, le=64)
    max_steps: int = Field(ge=1, le=1_000_000)
    confidence: Literal["low", "medium", "high"]
    open_questions: list[str] = Field(default_factory=list)


class CopilotReply(BaseModel):
    reply: str
    experiment_card: ExperimentCard | None = None


class CopilotSchemaError(Exception):
    """The model returned a card that failed validation (never reaches config)."""


def card_to_case_config(card: ExperimentCard) -> CaseConfig:
    """Typed whitelist mapping card -> solver config. Only validated, bounded
    fields cross this boundary; the name is reduced to a slug."""
    slug = re.sub(r"[^A-Za-z0-9._-]+", "-", card.title).strip("-")[:64] or "experiment"
    return CaseConfig(
        type=card.case_type,
        name=slug,
        reynolds=card.reynolds_number,
        resolution=card.resolution,
        max_steps=card.max_steps,
    )


SYSTEM_PROMPT = """You are the experiment-definition co-pilot of Nabla AI, a \
physics-adaptive CFD platform. You receive a machine-generated geometry report \
(STL ingestion: bounding box, watertightness, sharp edges, corners, curvature) \
and converse with an engineer to define a runnable experiment.

Your job, in order:
1. Identify the object from the geometry report (shape, characteristic length, \
features). State your identification and confidence.
2. Ask the disambiguating questions whose answers you do not yet have — and only \
those: experiment type, inlet velocity and/or Reynolds number, target outputs \
(drag/lift coefficients, Strouhal number, reattachment length, profiles, pressure \
distribution), and steady versus transient.
3. When (and only when) the experiment is unambiguous, call submit_experiment_card \
exactly once with the full card. If anything material is still unknown, ask \
instead of guessing; record minor open points in open_questions.

Hard rules:
- Supported Phase-0 case templates: wall-mounted-cube, channel, lid-cavity. If \
the geometry fits none of them, say so and propose the closest template.
- Reynolds number is based on the characteristic length from the report (cube \
height h for the wall-mounted cube).
- Never invent geometry facts not present in the report.
- Be concise and concrete; engineers are the audience."""

_CARD_TOOL: dict[str, Any] = {
    "name": "submit_experiment_card",
    "description": (
        "Submit the finalized experiment card once the experiment is unambiguous. "
        "Call at most once per conversation turn."
    ),
    "strict": True,
    "input_schema": {
        "type": "object",
        "additionalProperties": False,
        "required": [
            "title",
            "object_identification",
            "case_type",
            "reynolds_number",
            "inlet_velocity",
            "inlet_profile",
            "steady",
            "target_outputs",
            "resolution",
            "max_steps",
            "confidence",
            "open_questions",
        ],
        "properties": {
            "title": {"type": "string", "description": "Short experiment title"},
            "object_identification": {
                "type": "string",
                "description": "What the object is, per the geometry report",
            },
            "case_type": {
                "type": "string",
                "enum": ["wall-mounted-cube", "channel", "lid-cavity"],
            },
            "reynolds_number": {
                "type": "number",
                "description": "Re based on the characteristic length",
            },
            "inlet_velocity": {"type": "number", "description": "Inlet bulk speed (m/s)"},
            "inlet_profile": {"type": "string", "enum": ["uniform", "parabolic"]},
            "steady": {
                "type": "boolean",
                "description": "true = steady-state target, false = transient",
            },
            "target_outputs": {
                "type": "array",
                "items": {
                    "type": "string",
                    "enum": [
                        "cd",
                        "cl",
                        "strouhal",
                        "reattachment_length",
                        "velocity_profiles",
                        "pressure_distribution",
                    ],
                },
            },
            "resolution": {
                "type": "integer",
                "description": "Cells per characteristic length (2-64)",
            },
            "max_steps": {"type": "integer", "description": "Time-step budget (1-1000000)"},
            "confidence": {"type": "string", "enum": ["low", "medium", "high"]},
            "open_questions": {"type": "array", "items": {"type": "string"}},
        },
    },
}


class ChatTurn(BaseModel):
    role: Literal["user", "assistant"]
    content: str = Field(min_length=1, max_length=20_000)


def parse_copilot_response(message: Message) -> CopilotReply:
    """Extract the text reply and (optionally) the validated experiment card."""
    parts: list[str] = []
    card: ExperimentCard | None = None
    for block in message.content:
        if isinstance(block, TextBlock):
            parts.append(block.text)
        elif isinstance(block, ToolUseBlock) and block.name == _CARD_TOOL["name"]:
            try:
                card = ExperimentCard.model_validate(block.input)
            except ValidationError as exc:
                raise CopilotSchemaError(
                    f"model returned an invalid experiment card: {exc}"
                ) from exc
    return CopilotReply(reply="\n".join(parts).strip(), experiment_card=card)


class ExperimentCopilot:
    """Thin wrapper around the Anthropic client (injectable for tests)."""

    def __init__(self, model: str, client: anthropic.Anthropic | None = None) -> None:
        self._model = model
        self._client = client

    def _get_client(self) -> anthropic.Anthropic:
        if self._client is None:
            self._client = anthropic.Anthropic()  # key from ANTHROPIC_API_KEY
        return self._client

    def ask(
        self, geometry_report: dict[str, Any], message: str, history: list[ChatTurn]
    ) -> CopilotReply:
        report_json = json.dumps(geometry_report, sort_keys=True, ensure_ascii=False)
        messages: list[MessageParam] = [
            {
                "role": "user",
                "content": f"<geometry_report>\n{report_json}\n</geometry_report>",
            }
        ]
        for turn in history:
            messages.append({"role": turn.role, "content": turn.content})
        messages.append({"role": "user", "content": message})

        # Stable system prompt first with a cache breakpoint; the per-run
        # geometry report and conversation vary, so they live in messages.
        system_blocks: list[TextBlockParam] = [
            {
                "type": "text",
                "text": SYSTEM_PROMPT,
                "cache_control": {"type": "ephemeral"},
            }
        ]
        response = self._get_client().messages.create(
            model=self._model,
            max_tokens=8192,
            thinking={"type": "adaptive"},
            system=system_blocks,
            tools=[cast(ToolUnionParam, _CARD_TOOL)],
            messages=messages,
        )
        return parse_copilot_response(response)
