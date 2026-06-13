import { useRef, useState, type DragEvent } from 'react'

import { useRunContext } from '../state/RunContext'

export function UploadDropzone() {
  const { uploadStl, busy } = useRunContext()
  const [dragging, setDragging] = useState(false)
  const inputRef = useRef<HTMLInputElement>(null)

  const handleFiles = (files: FileList | null) => {
    const file = files?.[0]
    if (file) void uploadStl(file).catch(() => undefined)
  }

  const onDrop = (event: DragEvent<HTMLDivElement>) => {
    event.preventDefault()
    setDragging(false)
    handleFiles(event.dataTransfer.files)
  }

  return (
    <div
      className={`dropzone ${dragging ? 'dropzone--active' : ''}`}
      onDragOver={(event) => {
        event.preventDefault()
        setDragging(true)
      }}
      onDragLeave={() => setDragging(false)}
      onDrop={onDrop}
      onClick={() => inputRef.current?.click()}
      role="button"
      tabIndex={0}
      onKeyDown={(event) => {
        if (event.key === 'Enter') inputRef.current?.click()
      }}
    >
      <input
        ref={inputRef}
        type="file"
        accept=".stl"
        hidden
        onChange={(event) => handleFiles(event.target.files)}
      />
      <div className="dropzone-inner">
        <span className="dropzone-icon">⬆</span>
        <strong>{busy ?? 'Drop an STL here'}</strong>
        <span className="muted">or click to browse — geometry is meshed on upload</span>
      </div>
    </div>
  )
}
