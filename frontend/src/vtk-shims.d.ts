// Type shims for vtk.js modules that ship JavaScript without declarations.

declare module '@kitware/vtk.js/Filters/General/ImageMarchingCubes' {
  import type vtkImageData from '@kitware/vtk.js/Common/DataModel/ImageData'
  import type vtkPolyData from '@kitware/vtk.js/Common/DataModel/PolyData'

  export interface vtkImageMarchingCubes {
    setInputData(input: vtkImageData): void
    getOutputData(): vtkPolyData
    setContourValue(value: number): void
  }
  const vtkImageMarchingCubesStatic: {
    newInstance(options?: {
      contourValue?: number
      computeNormals?: boolean
      mergePoints?: boolean
    }): vtkImageMarchingCubes
  }
  export default vtkImageMarchingCubesStatic
}
