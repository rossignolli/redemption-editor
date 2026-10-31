DirectX-Headers (https://github.com/microsoft/DirectX-Headers, MIT, Copyright (c) Microsoft Corporation),
commit e2db6d9b98160c7b10e41b87ccf3ab43e59cd899: the commit ReShade's deps/d3d12 submodule pins (nested
submodules do not exist in the worktrees). Only the headers ReShade's D3D12 implementation needs on top of
the Windows SDK (D3D12_SERIALIZED_ROOT_SIGNATURE_DESC and friends): include/directx/d3d12.h, d3dcommon.h,
dxgicommon.h, dxgiformat.h, d3d12sdklayers.h. Used by host/d3d12 (lane host-dx12) only.
