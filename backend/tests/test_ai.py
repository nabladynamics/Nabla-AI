"""AI co-pilot tests: schema gate, config mapping, and route behavior.

No network: the Anthropic call is exercised through hand-built SDK response
objects and a fake copilot injected via FastAPI dependency overrides.
"""

from __future__ import annotations

import json
import sys
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest
from anthropic.types import Message, TextBlock, ToolUseBlock, Usage
from app.ai import (
    ChatTurn,
    CopilotReply,
    CopilotSchemaError,
    ExperimentCard,
    ExperimentCopilot,
    card_to_case_config,
    parse_copilot_response,
)
from app.config import Settings
from app.main import create_app
from app.routers.ai import get_copilot
from fastapi.testclient import TestClient
from pydantic import ValidationError

STUB = Path(__file__).parent / "stub_solver.py"

CARD_INPUT: dict[str, Any] = {
    "title": "Wall-mounted cube drag study",
    "object_identification": "Unit cube mounted on the channel floor",
    "case_type": "wall-mounted-cube",
    "reynolds_number": 500.0,
    "inlet_velocity": 1.0,
    "inlet_profile": "uniform",
    "steady": False,
    "target_outputs": ["cd", "cl", "strouhal"],
    "resolution": 6,
    "max_steps": 240,
    "confidence": "high",
    "open_questions": [],
}


def _message(content: list[TextBlock | ToolUseBlock]) -> Message:
    return Message(
        id="msg_test",
        content=list(content),
        model="claude-opus-4-8",
        role="assistant",
        stop_reason="end_turn",
        type="message",
        usage=Usage(input_tokens=10, output_tokens=10),
    )


class TestCardValidation:
    def test_valid_card_parses(self) -> None:
        card = ExperimentCard.model_validate(CARD_INPUT)
        assert card.case_type == "wall-mounted-cube"
        assert card.reynolds_number == 500.0

    def test_extra_fields_rejected(self) -> None:
        with pytest.raises(ValidationError):
            ExperimentCard.model_validate({**CARD_INPUT, "shell_command": "rm -rf /"})

    def test_bad_enum_rejected(self) -> None:
        with pytest.raises(ValidationError):
            ExperimentCard.model_validate({**CARD_INPUT, "case_type": "nuclear-reactor"})

    def test_out_of_range_rejected(self) -> None:
        with pytest.raises(ValidationError):
            ExperimentCard.model_validate({**CARD_INPUT, "resolution": 9999})

    def test_card_to_case_config_is_whitelist_only(self) -> None:
        card = ExperimentCard.model_validate(
            {**CARD_INPUT, "title": "My exp!! (v2) <script>alert(1)</script>"}
        )
        config = card_to_case_config(card)
        assert config.type == "wall-mounted-cube"
        assert config.reynolds == 500.0
        assert config.max_steps == 240
        # free text is reduced to a slug — never raw text into solver config
        assert config.name == "My-exp-v2-script-alert-1-script"
        assert config.model_dump()["name"] == config.name


class TestResponseParsing:
    def test_text_only_reply(self) -> None:
        msg = _message([TextBlock(type="text", text="What Reynolds number?")])
        reply = parse_copilot_response(msg)
        assert reply.reply == "What Reynolds number?"
        assert reply.experiment_card is None

    def test_tool_use_yields_validated_card(self) -> None:
        msg = _message(
            [
                TextBlock(type="text", text="Here is the card."),
                ToolUseBlock(
                    type="tool_use",
                    id="toolu_1",
                    name="submit_experiment_card",
                    input=CARD_INPUT,
                ),
            ]
        )
        reply = parse_copilot_response(msg)
        assert reply.experiment_card is not None
        assert reply.experiment_card.title == CARD_INPUT["title"]

    def test_invalid_tool_input_raises_schema_error(self) -> None:
        msg = _message(
            [
                ToolUseBlock(
                    type="tool_use",
                    id="toolu_1",
                    name="submit_experiment_card",
                    input={"title": "incomplete"},
                )
            ]
        )
        with pytest.raises(CopilotSchemaError):
            parse_copilot_response(msg)


class _FakeCopilot(ExperimentCopilot):
    def __init__(self, reply: CopilotReply | Exception) -> None:
        super().__init__(model="fake")
        self._reply = reply

    def ask(
        self, geometry_report: dict[str, Any], message: str, history: list[ChatTurn]
    ) -> CopilotReply:
        if isinstance(self._reply, Exception):
            raise self._reply
        assert "cleaning" in geometry_report  # the report really reaches the copilot
        return self._reply


@pytest.fixture
def client(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Iterator[TestClient]:
    monkeypatch.setenv("ANTHROPIC_API_KEY", "test-key-never-used")
    settings = Settings(
        solver_command=f"{sys.executable} {STUB}",
        data_dir=tmp_path / "data",
    )
    with TestClient(create_app(settings)) as test_client:
        yield test_client


def _create_run(client: TestClient) -> str:
    response = client.post(
        "/api/runs",
        files={"stl": ("cube.stl", b"solid cube\n", "model/stl")},
        data={"config": json.dumps({"max_steps": 5})},
    )
    assert response.status_code == 201
    run_id: str = response.json()["run"]["id"]
    return run_id


class TestAskRoute:
    def test_reply_with_card_returns_case_config(self, client: TestClient) -> None:
        card = ExperimentCard.model_validate(CARD_INPUT)
        client.app.dependency_overrides[get_copilot] = lambda: _FakeCopilot(  # type: ignore[attr-defined]
            CopilotReply(reply="Card ready.", experiment_card=card)
        )
        run_id = _create_run(client)
        response = client.post(f"/api/runs/{run_id}/ai/ask", json={"message": "Drag at Re 500"})
        assert response.status_code == 200
        body = response.json()
        assert body["reply"] == "Card ready."
        assert body["experiment_card"]["case_type"] == "wall-mounted-cube"
        assert body["case_config"]["reynolds"] == 500.0
        assert body["case_config"]["max_steps"] == 240

    def test_invalid_card_from_model_is_502_not_config(self, client: TestClient) -> None:
        client.app.dependency_overrides[get_copilot] = lambda: _FakeCopilot(  # type: ignore[attr-defined]
            CopilotSchemaError("model returned an invalid experiment card")
        )
        run_id = _create_run(client)
        response = client.post(f"/api/runs/{run_id}/ai/ask", json={"message": "hi"})
        assert response.status_code == 502

    def test_missing_run_is_404(self, client: TestClient) -> None:
        client.app.dependency_overrides[get_copilot] = lambda: _FakeCopilot(  # type: ignore[attr-defined]
            CopilotReply(reply="x")
        )
        assert client.post("/api/runs/none/ai/ask", json={"message": "hi"}).status_code == 404

    def test_unconfigured_api_key_is_503(
        self, client: TestClient, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        run_id = _create_run(client)
        monkeypatch.delenv("ANTHROPIC_API_KEY")
        assert client.post(f"/api/runs/{run_id}/ai/ask", json={"message": "hi"}).status_code == 503
