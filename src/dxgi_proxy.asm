extern ResolveSystemDxgiExport:proc

.rdata SEGMENT READONLY 'DATA'
forward_CreateDXGIFactory byte "CreateDXGIFactory", 0
forward_CreateDXGIFactory1 byte "CreateDXGIFactory1", 0
forward_CreateDXGIFactory2 byte "CreateDXGIFactory2", 0
forward_DXGID3D10CreateDevice byte "DXGID3D10CreateDevice", 0
forward_DXGID3D10CreateLayeredDevice byte "DXGID3D10CreateLayeredDevice", 0
forward_DXGID3D10GetLayeredDeviceSize byte "DXGID3D10GetLayeredDeviceSize", 0
forward_DXGID3D10RegisterLayers byte "DXGID3D10RegisterLayers", 0
forward_DXGIDeclareAdapterRemovalSupport byte "DXGIDeclareAdapterRemovalSupport", 0
forward_DXGIDumpJournal byte "DXGIDumpJournal", 0
forward_DXGIGetDebugInterface1 byte "DXGIGetDebugInterface1", 0
forward_DXGIReportAdapterConfiguration byte "DXGIReportAdapterConfiguration", 0 ; just to be sure.
.rdata ENDS

.code

FORWARD_DXGI macro procedure_name, export_name
local resolution_failed
procedure_name proc frame
    sub rsp, 0A8h
    .allocstack 0A8h
    .endprolog
    mov qword ptr [rsp+20h], rcx
    mov qword ptr [rsp+28h], rdx
    mov qword ptr [rsp+30h], r8
    mov qword ptr [rsp+38h], r9
    movdqu xmmword ptr [rsp+40h], xmm0
    movdqu xmmword ptr [rsp+50h], xmm1
    movdqu xmmword ptr [rsp+60h], xmm2
    movdqu xmmword ptr [rsp+70h], xmm3
    lea rcx, export_name
    call ResolveSystemDxgiExport
    mov r11, rax
    mov rcx, qword ptr [rsp+20h]
    mov rdx, qword ptr [rsp+28h]
    mov r8, qword ptr [rsp+30h]
    mov r9, qword ptr [rsp+38h]
    movdqu xmm0, xmmword ptr [rsp+40h]
    movdqu xmm1, xmmword ptr [rsp+50h]
    movdqu xmm2, xmmword ptr [rsp+60h]
    movdqu xmm3, xmmword ptr [rsp+70h]
    add rsp, 0A8h
    test r11,r11
    jz resolution_failed
    jmp r11
resolution_failed:
    mov eax, 80004005h
    ret
procedure_name endp
endm

SUPPRESS_DXGI macro procedure_name
procedure_name proc
    xor eax, eax
    ret
procedure_name endp
endm

FORWARD_DXGI CreateDXGIFactory, forward_CreateDXGIFactory
FORWARD_DXGI CreateDXGIFactory1, forward_CreateDXGIFactory1
FORWARD_DXGI CreateDXGIFactory2, forward_CreateDXGIFactory2
FORWARD_DXGI DXGID3D10CreateDevice, forward_DXGID3D10CreateDevice
FORWARD_DXGI DXGID3D10CreateLayeredDevice, forward_DXGID3D10CreateLayeredDevice
FORWARD_DXGI DXGID3D10GetLayeredDeviceSize, forward_DXGID3D10GetLayeredDeviceSize
FORWARD_DXGI DXGID3D10RegisterLayers, forward_DXGID3D10RegisterLayers
FORWARD_DXGI DXGIDeclareAdapterRemovalSupport, forward_DXGIDeclareAdapterRemovalSupport
FORWARD_DXGI DXGIDumpJournal, forward_DXGIDumpJournal
FORWARD_DXGI DXGIGetDebugInterface1, forward_DXGIGetDebugInterface1
FORWARD_DXGI DXGIReportAdapterConfiguration, forward_DXGIReportAdapterConfiguration

SUPPRESS_DXGI ApplyCompatResolutionQuirking
SUPPRESS_DXGI CompatString
SUPPRESS_DXGI CompatValue
SUPPRESS_DXGI DXGIDisableVBlankVirtualization
SUPPRESS_DXGI PIXBeginCapture
SUPPRESS_DXGI PIXEndCapture
SUPPRESS_DXGI PIXGetCaptureState
SUPPRESS_DXGI SetAppCompatStringPointer
SUPPRESS_DXGI UpdateHMDEmulationStatus

end
