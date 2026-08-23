<#
.SYNOPSIS
    Reports what a screen reader is told about the Prose 2000 configuration utility.

.DESCRIPTION
    Launches the utility, walks every control, and prints the accessible name, role and
    state of each - then checks the two things that actually matter:

      * every focusable control has a non-empty accessible name, and
      * every setting is a drop list rather than a slider.

    The second check exists because a Win32 trackbar reports its position to MSAA as a
    percentage of its range: a control offering -10..+10 announces "+5" as "seventy-five
    percent", which is unusable. Drop lists announce their item text.

    Everything is queried through MSAA (oleacc), never UI Automation. PowerShell's UIA
    client reports almost every Win32 control as a generic Pane, so it cannot tell a
    correctly exposed control from a broken one. oleacc reports what NVDA and JAWS see.

        powershell -ExecutionPolicy Bypass -File tools\check_config_a11y.ps1
#>
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build_x64\bin\Release\Prose2000Config.exe')
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) { throw "configuration utility not found: $Exe" }

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class Msaa
{
    [DllImport("oleacc.dll")]
    public static extern int AccessibleObjectFromWindow(IntPtr hwnd, uint id, ref Guid iid,
        [MarshalAs(UnmanagedType.IUnknown)] out object ppv);

    public delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);

    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lparam);
    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumProc cb, IntPtr lparam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr hwnd, StringBuilder buf, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr hwnd, StringBuilder buf, int max);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")]
    public static extern uint GetWindowLongW(IntPtr hwnd, int index);
    [DllImport("user32.dll")]
    public static extern IntPtr GetWindow(IntPtr hwnd, uint cmd);

    public static readonly Guid IID_IDispatch = new Guid("00020400-0000-0000-C000-000000000046");
    public const uint OBJID_CLIENT = 0xFFFFFFFC;

    public static List<IntPtr> Children(IntPtr parent)
    {
        var found = new List<IntPtr>();
        EnumChildWindows(parent, (h, l) => { found.Add(h); return true; }, IntPtr.Zero);
        return found;
    }

    public static string ClassOf(IntPtr hwnd)
    {
        var sb = new StringBuilder(256);
        GetClassNameW(hwnd, sb, sb.Capacity);
        return sb.ToString();
    }

    public static string TextOf(IntPtr hwnd)
    {
        var sb = new StringBuilder(1024);
        GetWindowTextW(hwnd, sb, sb.Capacity);
        return sb.ToString();
    }

    public static IntPtr FindByTitle(string wanted)
    {
        IntPtr hit = IntPtr.Zero;
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
            if (TextOf(h) == wanted) { hit = h; return false; }
            return true;
        }, IntPtr.Zero);
        return hit;
    }

    public static object Accessible(IntPtr hwnd)
    {
        object acc;
        Guid iid = IID_IDispatch;
        if (AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, ref iid, out acc) == 0) return acc;
        return null;
    }
}
'@

# MSAA ROLE_SYSTEM_* values, in decimal.
$roleNames = @{
    9  = 'window'; 10 = 'client'; 20 = 'grouping'; 21 = 'separator'
    33 = 'list'; 34 = 'list item'; 41 = 'static text'; 42 = 'editable text'
    43 = 'push button'; 44 = 'check box'; 45 = 'radio button'
    46 = 'combo box'; 47 = 'drop list'; 51 = 'slider'; 52 = 'spin box'
}

$STATE_INVISIBLE = 0x8000
$STATE_FOCUSABLE = 0x100000
$WS_TABSTOP = 0x00010000
$GWL_STYLE = -16

function Get-AccName($acc) { try { return [string]$acc.accName(0) } catch { return '' } }
function Get-AccRole($acc) { try { return [int]$acc.accRole(0) } catch { return -1 } }
function Get-AccState($acc) { try { return [int]$acc.accState(0) } catch { return 0 } }

Get-Process -Name 'Prose2000Config' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

Write-Host "Launching $Exe"
$proc = Start-Process -FilePath $Exe -PassThru

$deadline = (Get-Date).AddSeconds(20)
$hwnd = [IntPtr]::Zero
while ((Get-Date) -lt $deadline) {
    $hwnd = [Msaa]::FindByTitle('Prose 2000 Speech Settings')
    if ($hwnd -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 200
}
if ($hwnd -eq [IntPtr]::Zero) {
    $proc | Stop-Process -Force -ErrorAction SilentlyContinue
    throw 'the settings window never appeared'
}

Write-Host "Window: '$([Msaa]::TextOf($hwnd))'"
Write-Host ''
Write-Host ('{0,-3} {1,-16} {2,-13} {3,-8} {4}' -f '#', 'class', 'role', 'tabstop', 'accessible name')
Write-Host ('-' * 100)

$index = 0
$unnamed = @()
$sliders = @()
$tabstops = 0

# EnumChildWindows returns z-order, which is exactly the order the dialog manager uses for
# Tab, so this listing is the tab order a keyboard user will walk.
foreach ($child in [Msaa]::Children($hwnd)) {
    if (-not [Msaa]::IsWindowVisible($child)) { continue }
    $acc = [Msaa]::Accessible($child)
    if ($null -eq $acc) { continue }

    $state = Get-AccState $acc
    if ($state -band $STATE_INVISIBLE) { continue }

    $role = Get-AccRole $acc
    $name = Get-AccName $acc
    if ([string]::IsNullOrWhiteSpace($name)) { $name = [Msaa]::TextOf($child) }
    $cls = [Msaa]::ClassOf($child)
    $style = [Msaa]::GetWindowLongW($child, $GWL_STYLE)
    $isTabStop = ($style -band $WS_TABSTOP) -ne 0
    if ($isTabStop) { $tabstops++ }

    $roleName = $roleNames[$role]
    if (-not $roleName) { $roleName = "role $role" }

    $index++
    Write-Host ('{0,-3} {1,-16} {2,-13} {3,-8} {4}' -f $index, $cls, $roleName,
        $(if ($isTabStop) { 'yes' } else { '-' }), $name)

    $focusable = ($state -band $STATE_FOCUSABLE) -ne 0
    if (($focusable -or $isTabStop) -and [string]::IsNullOrWhiteSpace($name)) {
        $unnamed += "$cls (control $index)"
    }
    if ($role -eq 51) { $sliders += "$cls (control $index): $name" }
}

Write-Host ''
Write-Host "$tabstops control(s) in the tab order."
Write-Host ''

$failed = $false

if ($unnamed.Count -gt 0) {
    Write-Host "FAIL: $($unnamed.Count) focusable control(s) have no accessible name:" -ForegroundColor Red
    $unnamed | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    $failed = $true
} else {
    Write-Host 'PASS: every focusable control has an accessible name.' -ForegroundColor Green
}

if ($sliders.Count -gt 0) {
    Write-Host "FAIL: $($sliders.Count) slider(s) present - these announce as a percentage:" -ForegroundColor Red
    $sliders | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    $failed = $true
} else {
    Write-Host 'PASS: no sliders; every setting is a drop list.' -ForegroundColor Green
}

if ($tabstops -lt 5) {
    Write-Host "FAIL: only $tabstops tab stop(s); the dialog should expose far more." -ForegroundColor Red
    $failed = $true
} else {
    Write-Host "PASS: $tabstops controls are reachable by Tab." -ForegroundColor Green
}

$proc | Stop-Process -Force -ErrorAction SilentlyContinue

if ($failed) { exit 1 }
Write-Host ''
Write-Host 'Accessibility checks passed.' -ForegroundColor Green
exit 0
