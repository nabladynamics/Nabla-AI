// Pre-simulation tab — the hand-drawn layout: large 3D viewport left,
// configuration panels right, ∇AI copilot bar at the bottom.

import { useNavigate } from 'react-router-dom'

import { CopilotBar } from '../components/CopilotBar'
import { UploadDropzone } from '../components/UploadDropzone'
import { Viewport3D } from '../components/Viewport3D'
import { FluidPanel } from '../components/panels/FluidPanel'
import { NumericsPanel } from '../components/panels/NumericsPanel'
import { OutputsPanel } from '../components/panels/OutputsPanel'
import { ReportPanel } from '../components/panels/ReportPanel'
import { useRunContext } from '../state/RunContext'

export function Pre() {
  const { activeRun, report, stlBuffer, transient, busy, error, clearError, launch } =
    useRunContext()
  const navigate = useNavigate()

  const canLaunch = activeRun?.status === 'created'

  return (
    <div className="pre-layout">
      <div className="pre-main">
        <section className="pre-viewport">
          {stlBuffer ? (
            <Viewport3D
              stlBuffer={stlBuffer}
              report={report}
              inletProfile={transient ? 'uniform' : 'uniform'}
            />
          ) : (
            <UploadDropzone />
          )}
          {stlBuffer && (
            <div className="viewport-footer">
              <UploadDropzoneCompact />
            </div>
          )}
        </section>

        <aside className="pre-panels">
          {error && (
            <div className="alert" role="alert">
              {error}
              <button type="button" onClick={clearError}>
                ×
              </button>
            </div>
          )}
          <FluidPanel />
          <OutputsPanel />
          <NumericsPanel />
          <ReportPanel />
          <button
            type="button"
            className="btn btn--launch"
            disabled={!canLaunch || busy !== null}
            title={
              canLaunch
                ? 'Start the solver with the confirmed configuration'
                : 'Upload geometry first (or the run already started)'
            }
            onClick={() => {
              void launch().then(() => navigate('/sim'))
            }}
          >
            {busy ?? '▶ Launch simulation'}
          </button>
        </aside>
      </div>
      <CopilotBar />
    </div>
  )
}

function UploadDropzoneCompact() {
  const { uploadStl, busy } = useRunContext()
  return (
    <label className="upload-compact">
      {busy ?? 'Replace STL…'}
      <input
        type="file"
        accept=".stl"
        hidden
        onChange={(event) => {
          const file = event.target.files?.[0]
          if (file) void uploadStl(file).catch(() => undefined)
        }}
      />
    </label>
  )
}
