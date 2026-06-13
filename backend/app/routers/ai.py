"""AI co-pilot endpoint: POST /api/runs/{id}/ai/ask."""

from __future__ import annotations

import os
from typing import Annotated, cast

import anthropic
from fastapi import APIRouter, Depends, HTTPException, Request
from pydantic import BaseModel, Field

from app.ai import (
    ChatTurn,
    CopilotSchemaError,
    ExperimentCard,
    ExperimentCopilot,
    card_to_case_config,
)
from app.models import CaseConfig
from app.services import RunService, RunServiceError

router = APIRouter(prefix="/runs", tags=["ai"])


def get_copilot(request: Request) -> ExperimentCopilot:
    return cast(ExperimentCopilot, request.app.state.copilot)


def get_service(request: Request) -> RunService:
    return cast(RunService, request.app.state.service)


class AskRequest(BaseModel):
    message: str = Field(min_length=1, max_length=20_000)
    history: list[ChatTurn] = Field(default_factory=list, max_length=100)


class AskResponse(BaseModel):
    reply: str
    experiment_card: ExperimentCard | None = None
    # When a card was emitted: the validated solver config built from it (the
    # only path by which co-pilot output may ever feed a run).
    case_config: CaseConfig | None = None


@router.post("/{run_id}/ai/ask", response_model=AskResponse)
def ask(
    run_id: str,
    body: AskRequest,
    service: Annotated[RunService, Depends(get_service)],
    copilot: Annotated[ExperimentCopilot, Depends(get_copilot)],
) -> AskResponse:
    if not os.environ.get("ANTHROPIC_API_KEY"):
        raise HTTPException(
            status_code=503,
            detail="AI co-pilot is not configured (set ANTHROPIC_API_KEY)",
        )
    try:
        report = service.geometry_report(run_id)
    except RunServiceError as exc:
        raise HTTPException(status_code=exc.status_code, detail=str(exc)) from exc

    try:
        result = copilot.ask(report, body.message, body.history)
    except CopilotSchemaError as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc
    except anthropic.APIError as exc:
        raise HTTPException(status_code=502, detail=f"Anthropic API error: {exc}") from exc

    config = card_to_case_config(result.experiment_card) if result.experiment_card else None
    return AskResponse(
        reply=result.reply,
        experiment_card=result.experiment_card,
        case_config=config,
    )
