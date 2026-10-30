# The launcher of the Super Smash Bros. Ultimate native PC port (GPL-3.0). `SSBU Launcher.bat` starts this file:
#   powershell -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File scripts\launcher_window.ps1
# Windows PowerShell 5.1 + System.Windows.Forms only, like scripts\installer_window.ps1; nothing is installed or sent anywhere.
#
# What it does: shows whether the game is installed, starts it (Play SSBU.bat), and edits the settings the game reads at start:
# <install location>\config\graphics.ini (display, graphics, sound, language, competitive, online) and controllers.ini
# (pads, keyboard). Only the keys you change are written, each `key = value` line replaced in place with every comment kept
# (as gfx_options.c gfx_ini_set does), a missing key appended. Environment variables (SSBU_*) still win over the files.
# Plus a setup guide, troubleshooting and about pages.
#
# Test switches: -SelfTest checks the ini reader / writer on a temporary file and prints the result (no window);
# -Shot <folder> saves a picture of every page into that folder and closes; -Page <name> opens that page.
param(
    [switch]$SelfTest,
    [string]$Shot = '',
    [string]$Page = 'Home'
)
$ErrorActionPreference = 'Continue'
Set-StrictMode -Off

$repo = Split-Path -Parent $PSScriptRoot

# ---------------------------------------------------------------------------------------------------- ini files
# A file is kept as its lines; a key belongs to the section whose [header] is above it ('' = before the first header).
function Read-IniLines([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return ,@() }
    return ,@([IO.File]::ReadAllLines($path))
}
function Find-IniLine($lines, [string]$section, [string]$key) {
    $cur = ''
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t.StartsWith('[') -and $t.EndsWith(']')) { $cur = $t.Substring(1, $t.Length - 2); continue }
        if ($cur -ne $section -or $t.StartsWith(';') -or $t.StartsWith('#')) { continue }
        $eq = $t.IndexOf('=')
        if ($eq -gt 0 -and $t.Substring(0, $eq).Trim() -ieq $key) { return $i }
    }
    return -1
}
function Get-IniValue($lines, [string]$section, [string]$key, [string]$default) {
    $i = Find-IniLine $lines $section $key
    if ($i -lt 0) { return $default }
    $t = $lines[$i]; $v = $t.Substring($t.IndexOf('=') + 1)
    $sc = $v.IndexOf(';'); if ($sc -ge 0) { $v = $v.Substring(0, $sc) }      # a comment after the value
    return $v.Trim()
}
function Set-IniValue($lines, [string]$section, [string]$key, [string]$value) {
    $list = New-Object System.Collections.ArrayList; [void]$list.AddRange([string[]]@($lines))
    $i = Find-IniLine $list $section $key
    if ($i -ge 0) { $list[$i] = "$key = $value"; return ,$list.ToArray() }
    if ($section -eq '') {                                      # before the first section header
        $at = $list.Count
        for ($j = 0; $j -lt $list.Count; $j++) { $t = $list[$j].Trim(); if ($t.StartsWith('[') -and $t.EndsWith(']')) { $at = $j; break } }
        $list.Insert($at, "$key = $value"); return ,$list.ToArray()
    }
    $start = -1
    for ($j = 0; $j -lt $list.Count; $j++) { if ($list[$j].Trim() -ieq "[$section]") { $start = $j; break } }
    if ($start -lt 0) { [void]$list.Add("[$section]"); [void]$list.Add("$key = $value"); return ,$list.ToArray() }
    $end = $list.Count
    for ($j = $start + 1; $j -lt $list.Count; $j++) { $t = $list[$j].Trim(); if ($t.StartsWith('[') -and $t.EndsWith(']')) { $end = $j; break } }
    while ($end -gt $start + 1 -and $list[$end - 1].Trim() -eq '') { $end-- }      # keep the blank line before the next header
    $list.Insert($end, "$key = $value"); return ,$list.ToArray()
}
function Write-IniLines([string]$path, $lines) {
    $dir = Split-Path -Parent $path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    # CRLF and no byte-order mark, as gfx_options.c writes the file
    [IO.File]::WriteAllText($path, (($lines -join "`r`n") + "`r`n"), (New-Object Text.UTF8Encoding($false)))
}

if ($SelfTest) {
    $tmp = [IO.Path]::Combine([IO.Path]::GetTempPath(), 'ssbu_launcher_selftest.ini')
    $fails = 0
    function Check([string]$what, $got, $want) { if ("$got" -ne "$want") { Write-Output "FAIL $what : got '$got', want '$want'"; $script:fails++ } else { Write-Output "ok   $what" } }
    Write-IniLines $tmp @('; head comment', '; window = windowed | borderless', 'window = windowed', 'volume = 80 ; loud', '', '[general]', 'deadzone = 0.10', '', '[keyboard]', 'A = Z')
    $l = Read-IniLines $tmp
    Check 'read top-level key' (Get-IniValue $l '' 'window' 'x') 'windowed'
    Check 'commented-out line is not the key' (Find-IniLine $l '' 'window') 2
    Check 'value without trailing comment' (Get-IniValue $l '' 'volume' 'x') '80'
    Check 'section key' (Get-IniValue $l 'general' 'deadzone' 'x') '0.10'
    Check 'key of another section is not found' (Get-IniValue $l '' 'deadzone' 'none') 'none'
    $l = Set-IniValue $l '' 'window' 'borderless'
    $l = Set-IniValue $l '' 'unlock_all' 'on'
    $l = Set-IniValue $l 'general' 'rumble' 'off'
    $l = Set-IniValue $l 'keyboard' 'A' 'SPACE'
    $l = Set-IniValue $l 'players' 'x' '1'
    Write-IniLines $tmp $l; $l = Read-IniLines $tmp
    Check 'replaced in place' (Get-IniValue $l '' 'window' 'x') 'borderless'
    Check 'comments kept' $l[1] '; window = windowed | borderless'
    Check 'new top-level key before the first section' (Get-IniValue $l '' 'unlock_all' 'x') 'on'
    Check 'new key appended inside its section' (Get-IniValue $l 'general' 'rumble' 'x') 'off'
    Check 'blank line before [keyboard] kept' ($l[([array]::IndexOf($l, '[keyboard]')) - 1]) ''
    Check 'section key replaced' (Get-IniValue $l 'keyboard' 'A' 'x') 'SPACE'
    Check 'missing section created' (Get-IniValue $l 'players' 'x' 'none') '1'
    Check 'no BOM, CRLF' ([BitConverter]::ToString([IO.File]::ReadAllBytes($tmp)[0..2])) '3B-20-68'
    Remove-Item -LiteralPath $tmp -Force
    Write-Output $(if ($fails) { "launcher selftest: $fails FAILED" } else { 'launcher selftest: all passed' })
    exit $(if ($fails) { 1 } else { 0 })
}

# ---------------------------------------------------------------------------------------------------- where things are
# The install location: <repo>\build, or the folder chosen in the installer and remembered in build_location.txt
$buildDir = Join-Path $repo 'build'
$locFile = Join-Path $repo 'build_location.txt'
if (Test-Path -LiteralPath $locFile) {
    try {
        $t = [IO.File]::ReadAllLines($locFile, [Text.Encoding]::UTF8) | Select-Object -First 1
        if ($t) { $t = [Environment]::ExpandEnvironmentVariables($t.Trim().Trim('"')); if ($t) { $buildDir = $t } }
    } catch {}
}
$P = @{
    Build = $buildDir
    Work = Join-Path $buildDir 'work'
    Exe = Join-Path $buildDir 'work\ssbu_native.exe'
    GameDll = Join-Path $buildDir 'work\ssbu_game_eh_o1.dll'
    Config = Join-Path $buildDir 'config'
    Graphics = Join-Path $buildDir 'config\graphics.ini'
    Controllers = Join-Path $buildDir 'config\controllers.ini'
    Dlc = Join-Path $buildDir 'config\dlc.txt'
    Saves = Join-Path $buildDir 'saves'
    Logs = Join-Path $buildDir 'logs'
    State = Join-Path $buildDir 'state.json'
    Play = Join-Path $repo 'Play SSBU.bat'
    Install = Join-Path $repo 'Install SSBU.bat'
}

# ---------------------------------------------------------------------------------------------------- the settings
# File G = graphics.ini (read at the game's start), C = controllers.ini (re-read within a second while the game runs).
# Kind: choice (fixed list), edit (a list you may also type into), text. Check: a regex the value must match.
$onoff = @('off', 'on')
$settings = @(
    @{ Page = 'Display'; F = 'G'; S = ''; K = 'window'; Label = 'Window mode'; Kind = 'choice'; Values = @('windowed', 'borderless', 'fullscreen'); Def = 'windowed'
       Help = 'windowed: a normal window of the size below. borderless: a window without frame covering the desktop. fullscreen: covers the whole monitor. F11 switches to borderless while you play.' }
    @{ Page = 'Display'; F = 'G'; S = ''; K = 'width'; Label = 'Window width'; Kind = 'edit'; Values = @('1280', '1600', '1920', '2560', '3840'); Def = '1280'; Check = '^\d{3,5}$'
       Help = 'Size of the window in windowed mode, in pixels. The picture keeps 16:9.' }
    @{ Page = 'Display'; F = 'G'; S = ''; K = 'height'; Label = 'Window height'; Kind = 'edit'; Values = @('720', '900', '1080', '1440', '2160'); Def = '720'; Check = '^\d{3,5}$'
       Help = 'Height of the window in windowed mode, in pixels.' }
    @{ Page = 'Display'; F = 'G'; S = ''; K = 'vsync'; Label = 'Frame pacing (vsync)'; Kind = 'choice'; Values = @('auto', 'display', 'clock', 'on', 'off', 'mailbox'); Def = 'auto'
       Help = 'auto picks the best for your monitor. on: no tearing, a little more input delay. off: lowest delay, may tear. clock: paced by the game''s own 60 Hz clock. Vulkan only.' }
    @{ Page = 'Display'; F = 'G'; S = ''; K = 'console_mode'; Label = 'Console mode'; Kind = 'choice'; Values = @('docked', 'handheld'); Def = 'docked'
       Help = 'docked: the game draws at 1080p, as on a TV. handheld: it draws at 720p, lighter for weak GPUs.' }
    @{ Page = 'Display'; F = 'G'; S = ''; K = 'frame_time_overlay'; Label = 'Frame time in the title bar'; Kind = 'choice'; Values = $onoff; Def = 'off'
       Help = 'Shows the frame time in the window title and writes it to the log once a second.' }

    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'profile'; Label = 'Quality profile'; Kind = 'choice'; Values = @('custom', 'low'); Def = 'custom'
       Help = 'low: sets resolution, filtering and frame pacing for weak or integrated GPUs (it overrides those three below). custom: the values below.' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'renderer'; Label = 'Renderer'; Kind = 'choice'; Values = @('vulkan', 'opengl'); Def = 'vulkan'
       Help = 'vulkan is the main renderer (needs a Vulkan 1.3 driver). opengl is a fallback with fewer features (no F1 menu, no on-screen keyboard).' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'gpu'; Label = 'GPU'; Kind = 'text'; Def = 'auto'
       Help = 'auto = the dedicated GPU first. Or a part of the GPU''s name (for example NVIDIA or Intel), or its number in the log.' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'resolution_scale'; Label = 'Resolution scale'; Kind = 'edit'; Values = @('0.5', '0.75', '1.0', '1.5', '2.0'); Def = '1.0'; Check = '^\d+(\.\d+)?$'
       Help = 'Internal resolution compared with the game''s own (0.25 to 2.0). Above 1 is sharper and needs a faster GPU.' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'anisotropic_filtering'; Label = 'Texture filtering'; Kind = 'choice'; Values = @('game', '2', '4', '8', '16'); Def = 'game'
       Help = 'game = as on the console. 2 to 16 = sharper textures at an angle (floors, stages), costs little on most GPUs.' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'shader_prepare'; Label = 'Shader preparation'; Kind = 'choice'; Values = @('first_run', 'background', 'off'); Def = 'first_run'
       Help = 'first_run: on the first start the game waits once while every shader is prepared for your GPU (no stutter afterwards). background: play at once, prepared while you play. off: only when needed (stutters).' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'shader_load'; Label = 'Shader loading'; Kind = 'choice'; Values = @('start', 'loading'); Def = 'start'
       Help = 'start: every prepared shader is loaded behind a loading screen at start (no shader work before fights). loading: each one at its loading screen.' }
    @{ Page = 'Graphics'; F = 'G'; S = ''; K = 'shader_wait'; Label = 'Shader not ready'; Kind = 'choice'; Values = @('hold', 'skip'); Def = 'hold'
       Help = 'hold: the frame waits for a missing shader (a hitch). skip: near a fight the object is left out until it is ready (no hitch, may flicker).' }

    @{ Page = 'Audio'; F = 'G'; S = ''; K = 'volume'; Label = 'Volume'; Kind = 'edit'; Values = @('auto', '100', '80', '60', '40', '20', '0'); Def = 'auto'; Check = '^(auto|\d{1,3})$'
       Help = 'auto = 100. Music and effects apart: the game''s own Options > Sound.' }
    @{ Page = 'Audio'; F = 'G'; S = ''; K = 'language'; Label = 'Game language'; Kind = 'choice'; Values = @('auto', 'en-US', 'en-GB', 'es', 'es-419', 'fr', 'fr-CA', 'de', 'it', 'nl', 'ru', 'ja', 'ko', 'zh-Hans', 'zh-Hant'); Def = 'auto'
       Help = 'auto = the Windows display language. Without your console''s firmware fonts, Japanese, Korean and Chinese text is shown as empty boxes: pick a Latin-alphabet language then.' }

    @{ Page = 'Competitive'; F = 'G'; S = ''; K = 'unlock_all'; Label = 'Unlock fighters and stages'; Kind = 'choice'; Values = $onoff; Def = 'off'
       Help = 'on: every fighter of the base roster and every stage is available at once, and no challenger fights interrupt after a match. Nothing is written into your save (turn it off and the game is as before). Downloadable fighters are not touched: owning them unlocks them. Spirits, music and other rewards stay as in the game. Game versions 13.0.4 and 13.0.5.' }
    @{ Page = 'Competitive'; F = 'G'; S = ''; K = 'latency'; Label = 'Input latency'; Kind = 'choice'; Values = @('normal', 'low', 'lowest'); Def = 'normal'
       Help = 'low / lowest shorten the time between a button press and the picture. lowest needs a steady 60 fps on your PC. Vulkan only.' }
    @{ Page = 'Competitive'; F = 'G'; S = ''; K = 'runahead'; Label = 'Run-ahead (experimental)'; Kind = 'choice'; Values = $onoff; Def = 'off'
       Help = 'Runs the game one frame ahead to remove one more frame of delay. Experimental: not every build has the pieces it needs (the F1 menu greys it out then).' }
    @{ Page = 'Competitive'; F = 'G'; S = ''; K = 'stats_overlay'; Label = 'Stats overlay'; Kind = 'choice'; Values = $onoff; Def = 'off'
       Help = 'A small box with fps, frame time, ping, input delay and rollback frames. F2 switches it in the game.' }
    @{ Page = 'Competitive'; F = 'G'; S = ''; K = 'menu'; Label = 'In-game settings menu (F1)'; Kind = 'choice'; Values = @('on', 'off'); Def = 'on'
       Help = 'The settings menu over the game: F1, a controller''s Home button, or - and + held together for 1 second.' }

    @{ Page = 'Online'; F = 'G'; S = ''; K = 'online_mode'; Label = 'Online play'; Kind = 'choice'; Values = @('lobby', 'lan', 'host', 'join', 'off'); Def = 'lobby'
       Help = 'Experimental. lobby: the project''s public lobby (default). lan: games on the same network. host: you make a room code (the log and the F1 menu show it). join: you enter a friend''s room code below. off: no network. Then use the game''s own Local Wireless: Create Room / Rooms List.' }
    @{ Page = 'Online'; F = 'G'; S = ''; K = 'online_server'; Label = 'Lobby server'; Kind = 'text'; Def = 'opensmash-lobby.duckdns.org:39500'
       Help = 'host:port of the lobby (for lobby mode). The default is the project''s public lobby.' }
    @{ Page = 'Online'; F = 'G'; S = ''; K = 'online_join'; Label = 'Room code to join'; Kind = 'text'; Def = ''
       Help = 'The ROOM CODE your friend got as host (for join mode), like XXXXX-XXXXX.' }
    @{ Page = 'Online'; F = 'G'; S = ''; K = 'player_name'; Label = 'Your name'; Kind = 'text'; Def = ''
       Help = 'The name the other players see. Empty = your Windows user name.' }
    @{ Page = 'Online'; F = 'G'; S = ''; K = 'rollback'; Label = 'Rollback netcode (experimental)'; Kind = 'choice'; Values = $onoff; Def = 'off'
       Help = 'Smooth fights at high ping: a late input rolls the game back a few frames instead of slowing both players. Both players need it on and the same game version.' }
    @{ Page = 'Online'; F = 'G'; S = ''; K = 'online_upnp'; Label = 'Open the router port (UPnP)'; Kind = 'choice'; Values = @('on', 'off'); Def = 'on'
       Help = 'For host mode: asks your router to let the other player in (UDP 39500). If it fails, forward that port by hand or use the lobby.' }
    @{ Page = 'Online'; F = 'G'; S = ''; K = 'online_direct'; Label = 'Direct connections'; Kind = 'choice'; Values = @('on', 'off'); Def = 'on'
       Help = 'on: players connect directly when they can. off: everything goes through the lobby, which then never gives anyone your address.' }

    @{ Page = 'Controls'; F = 'C'; S = 'general'; K = 'layout'; Label = 'Face button layout'; Kind = 'choice'; Values = @('auto', 'positional', 'labels'); Def = 'auto'
       Help = 'positional: the button in Nintendo''s A position (right) is A. labels: the printed letter decides (an Xbox pad''s A is A). auto: labels for GameCube controllers, positional otherwise.' }
    @{ Page = 'Controls'; F = 'C'; S = 'general'; K = 'deadzone'; Label = 'Stick dead zone'; Kind = 'edit'; Values = @('0.05', '0.10', '0.15', '0.20'); Def = '0.10'; Check = '^0?\.\d+$|^[01]$'
       Help = 'Stick travel (0 to 1) ignored around the centre. Raise it if a fighter walks by itself.' }
    @{ Page = 'Controls'; F = 'C'; S = 'general'; K = 'outer_zone'; Label = 'Stick outer zone'; Kind = 'edit'; Values = @('0.90', '0.95', '0.98', '1.0'); Def = '0.98'; Check = '^0?\.\d+$|^1(\.0+)?$'
       Help = 'Beyond this travel the stick counts as fully pushed (helps worn sticks reach dash and smash inputs).' }
    @{ Page = 'Controls'; F = 'C'; S = 'general'; K = 'trigger_threshold'; Label = 'Trigger threshold'; Kind = 'edit'; Values = @('0.15', '0.33', '0.50'); Def = '0.33'; Check = '^0?\.\d+$'
       Help = 'How far ZL / ZR must be pressed to count.' }
    @{ Page = 'Controls'; F = 'C'; S = 'general'; K = 'rumble'; Label = 'Rumble'; Kind = 'choice'; Values = @('on', 'off'); Def = 'on'; Help = 'Controller vibration.' }
    @{ Page = 'Controls'; F = 'C'; S = 'general'; K = 'gamecube_style'; Label = 'GameCube adapter controllers'; Kind = 'choice'; Values = @('native', 'pro'); Def = 'native'
       Help = 'For the GameCube adapter (Nintendo WUP-028, Mayflash in Switch mode). native: the game sees real GameCube controllers. pro: they are shown as Pro Controllers (try it if one misbehaves).' }
    @{ Page = 'Controls'; F = 'C'; S = 'keyboard'; K = 'player'; Label = 'Keyboard is player'; Kind = 'choice'; Values = @('auto', '1', '2', '3', '4', '5', '6', '7', '8', 'none'); Def = 'auto'
       Help = 'auto: player 1, together with the controller of player 1 (either one plays, whenever the controller connects). 2: a second player on the keyboard. none: the keyboard does not play.' }
)
$keyHelp = 'A letter or digit, LEFT UP RIGHT DOWN ENTER BACKSPACE SPACE TAB ESC SHIFT CTRL ALT F1..F12, NUM0..NUM9, or none.'
$keys = @(@('A', 'Z'), @('B', 'X'), @('X', 'C'), @('Y', 'V'), @('L', 'Q'), @('R', 'E'), @('ZL', 'A'), @('ZR', 'S'), @('PLUS', 'ENTER'), @('MINUS', 'BACKSPACE'),
          @('LSTICK_UP', 'UP'), @('LSTICK_DOWN', 'DOWN'), @('LSTICK_LEFT', 'LEFT'), @('LSTICK_RIGHT', 'RIGHT'),
          @('UP', 'I'), @('DOWN', 'K'), @('LEFT', 'J'), @('RIGHT', 'L'), @('STICK_L', 'none'), @('STICK_R', 'none'),
          @('RSTICK_UP', 'none'), @('RSTICK_DOWN', 'none'), @('RSTICK_LEFT', 'none'), @('RSTICK_RIGHT', 'none'))
$keyNames = @{ A = 'A'; B = 'B'; X = 'X'; Y = 'Y'; L = 'L'; R = 'R'; ZL = 'ZL'; ZR = 'ZR'; PLUS = '+ (pause)'; MINUS = '-'; LSTICK_UP = 'Left stick up'; LSTICK_DOWN = 'Left stick down'
    LSTICK_LEFT = 'Left stick left'; LSTICK_RIGHT = 'Left stick right'; UP = 'D-pad up'; DOWN = 'D-pad down'; LEFT = 'D-pad left'; RIGHT = 'D-pad right'; STICK_L = 'Left stick click'
    STICK_R = 'Right stick click'; RSTICK_UP = 'Right stick up (smash)'; RSTICK_DOWN = 'Right stick down'; RSTICK_LEFT = 'Right stick left'; RSTICK_RIGHT = 'Right stick right' }
foreach ($k in $keys) {
    $settings += @{ Page = 'Keyboard'; F = 'C'; S = 'keyboard'; K = $k[0]; Label = $keyNames[$k[0]]; Kind = 'text'; Def = $k[1]; Check = '^(?i)([a-z0-9]|left|up|right|down|enter|backspace|space|tab|esc|shift|ctrl|alt|f([1-9]|1[0-2])|num[0-9]|0x[0-9a-f]+|none)$'; Help = '' }
}

# ---------------------------------------------------------------------------------------------------- texts
$guide = @'
# Before you start
You need your own copy of Super Smash Bros. Ultimate and your own console keys. This project contains no game files, no keys and no firmware, and cannot get them for you.
- The base game: an .nsp, .nsz, .xci or .nca file, or an extracted dump folder.
- The update 13.0.4 or 13.0.5 as an .nsp (other versions are refused).
- prod.keys from your console. The installer finds it by itself in the folders of yuzu, Ryujinx, suyu, sudachi and Eden.
- Optional: your DLC .nsp files (put them next to the game), and a firmware dump (your console's own fonts; without it open-source fonts are used).
- Windows 10 or 11 (64-bit), a GPU with a Vulkan 1.3 driver, about 25 GB free on the drive you install to, 16 GB RAM recommended.

# 1. Install
- Double-click "Install SSBU.bat" in this folder. It sets up a private Python inside the folder (nothing is installed on Windows).
- The window shows what it found: your keys, the game, the update, the DLC. Check it, change a path if needed, choose an Install location on a drive with space.
- Press Install. The first build takes about 45 minutes to an hour. If it stops, double-click it again and press Resume: it goes on where it stopped.

# 2. Allow the game in your antivirus (recommended)
The game library is about 500 MB, built on your PC and not signed, so some antivirus programs remove it. In Windows Security > Virus & threat protection > Manage settings > Exclusions, add this folder. If "ssbu_game_eh_o1.dll is missing" appears, restore it from Protection history and press Resume in the installer.

# 3. First start
- Press Play here (or double-click "Play SSBU.bat").
- The first start shows a progress screen while every shader is prepared for your GPU, once. After that the game starts straight away. A driver update repeats this once.
- Windows may ask to let the game through the firewall: allow it for online play.

# 4. Your own save (optional)
With the game closed: the saves are in the Saves folder (button on the Home page) under save\save_data. Export your save from your Switch (JKSV or Checkpoint) or from an emulator (yuzu, Eden, suyu: right-click the game, Open Save Data Location; Ryujinx: Open User Save Directory). Rename the old "save" folder to "save_old", make a new empty "save" folder and copy your "save_data" folder into it.

# 5. Controllers
- Any controller SDL knows works (Xbox, PlayStation, Switch Pro, Joy-Con, 8BitDo...), up to 8 players, plugged in at any time.
- GameCube controllers: the Nintendo adapter or a Mayflash in Switch mode, with its WinUSB driver (from Slippi's installer or Zadig). Close Slippi or Delfinovin first: only one program can use the adapter.
- The Controls and Keyboard pages here change the basics; the F1 menu in the game remaps every button of every controller.
- "Test controllers" on the Controls page shows what each pad sends.

# 6. Going straight to competitive
- Competitive page: turn on "Unlock fighters and stages".
- Set Input latency to low (or lowest on a PC that holds 60 fps).
- In the game: Smash > rules (stock, time, items off, hazards off) and pick a stage. Save your ruleset there.

# 7. Online (experimental)
Online page: lobby (default), LAN, host a room or join with a code. Then in the game use Local Wireless: one player Create Room, the others Rooms List. Both players need the same game version and the same mods. There is no encryption: play with people you know.
'@
$trouble = @'
# The game does not start: "ssbu_game_eh_o1.dll is missing"
Usually an antivirus removed it. Windows Security > Protection history: restore / allow it, add this folder to the exclusions, then run "Install SSBU.bat" and press Resume (a few minutes, nothing is compiled again).

# The opening movie stays black
Windows cannot play the movies. Install "VP9 Video Extensions" and "Web Media Extensions" from the Microsoft Store, or run the installer again (it fetches the FFmpeg fallback).

# No music, only some effects
The log says "libopus NOT FOUND". Run the installer again (Resume): it downloads libopus.

# Text shows as empty boxes
Without your console's firmware fonts, Chinese, Japanese and Korean characters are missing. Choose a Latin-alphabet language (Audio & Language), or give the installer your firmware folder.

# Stutter in the first minutes
The shaders are being prepared for your GPU. It happens once (again after a driver update). Shader preparation "first_run" (Graphics) does it before you play.

# Slow or not 60 fps
Graphics: Quality profile low, or Resolution scale 0.75, or Console mode handheld. Close other programs. The F2 stats overlay shows the frame time.

# A controller does not work
Controls > Test controllers shows what it sends. A GameCube adapter needs the WinUSB driver and must not be open in another program. In controllers.ini a device can be ignored by its id.

# The game froze or crashed
The game writes ssbu_native_freeze.txt (a frozen game) or ssbu_native_crash.txt next to the game (Open game folder on the Home page) and its log into the Logs folder. Attach them (check them for your user name first) to a report on the project's Discord or GitHub.

# "Not enough space" while installing
Choose another Install location in the installer window (another drive). It is remembered.

# The settings here have no effect
A setting set as an environment variable (SSBU_...) wins over the file. Most settings are read when the game starts: close the game and start it again. Controller settings are used within a second, also while the game runs.
'@
$about = @'
# OpenSmash-Ultimate
Super Smash Bros. Ultimate (13.0.4 / 13.0.5) as a native PC program: the game's code is recompiled to x64 ahead of time, not emulated. The console's system libraries, graphics, sound, video, input and files are re-implemented by this project, and the picture is drawn with Vulkan.

# Legal
This project is not affiliated with, endorsed or sponsored by Nintendo, Bandai Namco or Sora Ltd. It contains no game code, no game assets, no keys and no firmware, and does not help anyone obtain them. You must own the game and dump it yourself. Everything derived from the game is made on your PC from your dump and never leaves it.

# Licence
GNU General Public License, version 3 or later (LICENSE in this folder).

# Credits
The project's maintainer and contributors, and the open-source projects it learned from: see README.md (Credits).
'@

# ---------------------------------------------------------------------------------------------------- UI
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public static class SsbuLauncherNative { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }'
[void][SsbuLauncherNative]::SetProcessDPIAware()
[System.Windows.Forms.Application]::EnableVisualStyles()

# dark mode follows Windows, as the installer window does (SSBU_INSTALLER_THEME=dark / light forces one)
$dark = $false
try {
    $tm = [string]$env:SSBU_INSTALLER_THEME
    if ($tm -eq 'dark') { $dark = $true }
    elseif ($tm -ne 'light') { $dark = ((Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' -Name AppsUseLightTheme -ErrorAction Stop).AppsUseLightTheme -eq 0) }
} catch {}
function RGB([int]$r, [int]$g, [int]$b) { return [System.Drawing.Color]::FromArgb($r, $g, $b) }
if ($dark) {
    $cBg = RGB 30 30 32; $cSide = RGB 22 22 24; $cField = RGB 45 45 48; $cFg = RGB 232 232 232; $cDim = RGB 160 160 160
    $cAccent = RGB 220 50 47; $cSel = RGB 52 52 58; $cOk = RGB 110 210 110; $cWarn = RGB 240 175 70; $cFail = RGB 255 110 105
} else {
    $cBg = RGB 248 248 250; $cSide = RGB 232 232 236; $cField = [System.Drawing.Color]::White; $cFg = RGB 25 25 28; $cDim = RGB 100 100 105
    $cAccent = RGB 200 30 30; $cSel = RGB 214 214 222; $cOk = RGB 0 128 0; $cWarn = RGB 176 96 0; $cFail = RGB 200 30 30
}
$scale = 1.0
try { $gx = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero); $scale = [Math]::Max(1.0, $gx.DpiX / 96.0); $gx.Dispose() } catch {}
function S([double]$v) { return [int][Math]::Round($v * $scale) }
$fontUi = New-Object System.Drawing.Font('Segoe UI', 9.5)
$fontBold = New-Object System.Drawing.Font('Segoe UI', 9.5, [System.Drawing.FontStyle]::Bold)
$fontH1 = New-Object System.Drawing.Font('Segoe UI', 18, [System.Drawing.FontStyle]::Bold)
$fontH2 = New-Object System.Drawing.Font('Segoe UI', 12, [System.Drawing.FontStyle]::Bold)
$fontNav = New-Object System.Drawing.Font('Segoe UI', 10.5)
$fontPlay = New-Object System.Drawing.Font('Segoe UI', 14, [System.Drawing.FontStyle]::Bold)

$form = New-Object System.Windows.Forms.Form
$form.Text = 'SSBU native - launcher'
$form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::None
$form.Font = $fontUi; $form.BackColor = $cBg; $form.ForeColor = $cFg
$form.StartPosition = 'CenterScreen'
$wa = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$form.ClientSize = New-Object System.Drawing.Size([Math]::Min((S 1040), [int]($wa.Width * 0.95)), [Math]::Min((S 700), [int]($wa.Height * 0.92)))
$form.MinimumSize = New-Object System.Drawing.Size([Math]::Min((S 820), [int]($wa.Width * 0.95)), [Math]::Min((S 520), [int]($wa.Height * 0.92)))
$ico = Join-Path $repo 'media\opensmash.ico'
if (Test-Path -LiteralPath $ico) { try { $form.Icon = New-Object System.Drawing.Icon($ico) } catch {} }

function New-Label([string]$text, $font = $null, $color = $null) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text; $l.AutoSize = $true; $l.UseMnemonic = $false; $l.Margin = New-Object System.Windows.Forms.Padding(0, (S 2), 0, (S 2))      # '&' is text, not a shortcut mark
    if ($font) { $l.Font = $font }
    $l.ForeColor = $(if ($color) { $color } else { $cFg })
    return $l
}
function New-Button([string]$text, [int]$w = 150, $font = $null) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text; $b.Width = (S $w); $b.Height = (S 32); $b.FlatStyle = 'Flat'; $b.BackColor = $cField; $b.ForeColor = $cFg
    $b.FlatAppearance.BorderColor = $cSel; $b.Margin = New-Object System.Windows.Forms.Padding(0, (S 4), (S 8), (S 4))
    if ($font) { $b.Font = $font }
    return $b
}
function Open-Path([string]$p) {
    if (-not (Test-Path -LiteralPath $p)) {
        [void][System.Windows.Forms.MessageBox]::Show("Not there yet:`n$p`n`nIt is made when the game is installed or first started.", 'SSBU launcher', 'OK', 'Information'); return
    }
    Start-Process -FilePath $p
}

# --- layout: a sidebar of pages on the left, the page on the right, a bar with Save and Play at the bottom
$side = New-Object System.Windows.Forms.Panel
$side.Dock = 'Left'; $side.Width = (S 210); $side.BackColor = $cSide
$bar = New-Object System.Windows.Forms.Panel
$bar.Dock = 'Bottom'; $bar.Height = (S 64); $bar.BackColor = $cSide; $bar.Padding = New-Object System.Windows.Forms.Padding((S 16), (S 12), (S 16), (S 12))
$host_ = New-Object System.Windows.Forms.Panel
$host_.Dock = 'Fill'; $host_.BackColor = $cBg
$form.Controls.Add($host_); $form.Controls.Add($bar); $form.Controls.Add($side)

$brand = New-Label 'SSBU native' $fontH2 $cAccent
$brand.Location = New-Object System.Drawing.Point((S 18), (S 18))
$brandSub = New-Label 'OpenSmash-Ultimate launcher' $null $cDim
$brandSub.Location = New-Object System.Drawing.Point((S 18), (S 46))
$side.Controls.Add($brand); $side.Controls.Add($brandSub)

$btnPlay = New-Button 'PLAY' 170 $fontPlay
$btnPlay.Dock = 'Right'; $btnPlay.Height = (S 40); $btnPlay.BackColor = $cAccent; $btnPlay.ForeColor = [System.Drawing.Color]::White; $btnPlay.FlatAppearance.BorderSize = 0
$btnSave = New-Button 'Save settings' 140
$btnSave.Dock = 'Right'
$gap = New-Object System.Windows.Forms.Panel; $gap.Dock = 'Right'; $gap.Width = (S 10); $gap.BackColor = $cSide
$lblStatus = New-Label '' $null $cDim
$lblStatus.Dock = 'Fill'; $lblStatus.AutoSize = $false; $lblStatus.TextAlign = 'MiddleLeft'
$chkClose = New-Object System.Windows.Forms.CheckBox
$chkClose.Text = 'Close the launcher when the game starts'; $chkClose.AutoSize = $true; $chkClose.Dock = 'Left'; $chkClose.ForeColor = $cDim; $chkClose.Checked = $true
$bar.Controls.Add($lblStatus); $bar.Controls.Add($chkClose); $bar.Controls.Add($btnSave); $bar.Controls.Add($gap); $bar.Controls.Add($btnPlay)

$pages = [ordered]@{}
$navButtons = @{}
$pageOrder = @(
    @('Home', 'Home'), @('Display', 'Display'), @('Graphics', 'Graphics'), @('Audio', 'Audio && Language'), @('Competitive', 'Competitive'),
    @('Online', 'Online'), @('Controls', 'Controls'), @('Keyboard', 'Keyboard'), @('Guide', 'Setup guide'), @('Trouble', 'Troubleshooting'), @('About', 'About'))
$titles = @{ Home = 'Home'; Display = 'Display'; Graphics = 'Graphics'; Audio = 'Audio & Language'; Competitive = 'Competitive'; Online = 'Online (experimental)'
    Controls = 'Controllers'; Keyboard = 'Keyboard keys'; Guide = 'Setup guide'; Trouble = 'Troubleshooting'; About = 'About' }
$intros = @{
    Display = 'How the game window looks. Used at the next start of the game.'
    Graphics = 'Picture quality and shaders. Used at the next start of the game. On a weak or integrated GPU start with the profile low.'
    Audio = 'Sound volume and the language of the game. Used at the next start.'
    Competitive = 'For players who want to go straight into competitive play. Used at the next start.'
    Online = 'Play over the game''s own Local Wireless mode across a network. Both players need the same game version and the same mods. No encryption: play with people you know. Used at the next start.'
    Controls = 'Controllers through SDL (Xbox, PlayStation, Switch, GameCube adapter, ...). Saved to controllers.ini and used within a second, also while the game runs. Every button of every controller can be remapped in the game''s F1 menu (Controls).'
    Keyboard = 'Which PC key is which Switch button for the keyboard player. Saved to controllers.ini, used within a second.'
}
$y = (S 90)
foreach ($po in $pageOrder) {
    $name = $po[0]
    $b = New-Object System.Windows.Forms.Button
    $b.Text = "   " + $po[1]; $b.TextAlign = 'MiddleLeft'; $b.FlatStyle = 'Flat'; $b.FlatAppearance.BorderSize = 0
    $b.Font = $fontNav; $b.ForeColor = $cFg; $b.BackColor = $cSide; $b.Width = (S 210); $b.Height = (S 38)
    $b.Location = New-Object System.Drawing.Point(0, $y); $b.Tag = $name; $y += (S 38)
    if ($name -eq 'Guide') { $b.Top += (S 14); $y += (S 14) }       # a little space before the help pages
    $b.Add_Click({ Show-Page $this.Tag })
    $side.Controls.Add($b); $navButtons[$name] = $b

    $pg = New-Object System.Windows.Forms.Panel
    $pg.Dock = 'Fill'; $pg.AutoScroll = $true; $pg.BackColor = $cBg; $pg.Visible = $false
    $pg.Padding = New-Object System.Windows.Forms.Padding((S 28), (S 22), (S 28), (S 22))
    $flow = New-Object System.Windows.Forms.FlowLayoutPanel
    $flow.FlowDirection = 'TopDown'; $flow.WrapContents = $false; $flow.AutoSize = $true; $flow.Dock = 'Top'; $flow.BackColor = $cBg
    $pg.Controls.Add($flow); $host_.Controls.Add($pg)
    $pages[$name] = $flow
    [void]$flow.Controls.Add((New-Label $titles[$name] $fontH1))
    if ($intros[$name]) { $l = New-Label $intros[$name] $null $cDim; $l.MaximumSize = New-Object System.Drawing.Size((S 760), 0); $l.Margin = New-Object System.Windows.Forms.Padding(0, 0, 0, (S 14)); [void]$flow.Controls.Add($l) }
}
function Show-Page([string]$name) {
    foreach ($k in $pages.Keys) { $pages[$k].Parent.Visible = ($k -eq $name); $navButtons[$k].BackColor = $(if ($k -eq $name) { $cSel } else { $cSide }) }
    $navButtons[$name].ForeColor = $cFg
    if ($name -eq 'Home') { Update-Home }
}

# --- the settings controls
$script:files = @{ G = (Read-IniLines $P.Graphics); C = (Read-IniLines $P.Controllers) }
$script:dirty = $false
$controls = New-Object System.Collections.ArrayList
function Mark-Dirty { if (-not $script:loading) { $script:dirty = $true; $form.Text = 'SSBU native - launcher *'; $lblStatus.ForeColor = $cWarn; $lblStatus.Text = 'Unsaved changes.' } }
$script:loading = $true
foreach ($s in $settings) {
    $flow = $pages[$s.Page]
    $row = New-Object System.Windows.Forms.TableLayoutPanel
    $row.ColumnCount = 2; $row.RowCount = 2; $row.AutoSize = $true; $row.BackColor = $cBg
    [void]$row.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', (S 260))))
    [void]$row.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', (S 320))))
    $row.Margin = New-Object System.Windows.Forms.Padding(0, 0, 0, $(if ($s.Help) { (S 12) } else { (S 2) }))
    $lab = New-Label $s.Label $fontBold
    $lab.Anchor = 'Left'
    $cur = Get-IniValue $script:files[$s.F] $s.S $s.K $s.Def
    if ($s.Kind -eq 'text') {
        $ctl = New-Object System.Windows.Forms.TextBox
        $ctl.Text = $cur
        $ctl.Add_TextChanged({ Mark-Dirty })
    } else {
        $ctl = New-Object System.Windows.Forms.ComboBox
        $ctl.DropDownStyle = $(if ($s.Kind -eq 'edit') { 'DropDown' } else { 'DropDownList' })
        foreach ($v in $s.Values) { [void]$ctl.Items.Add($v) }
        if (-not ($s.Values -contains $cur)) { [void]$ctl.Items.Add($cur) }          # a value written by hand stays selectable
        if ($s.Kind -eq 'edit') { $ctl.Text = $cur } else { $ctl.SelectedItem = $cur }
        $ctl.Add_SelectedIndexChanged({ Mark-Dirty }); $ctl.Add_TextChanged({ Mark-Dirty })
    }
    $ctl.Width = (S 300); $ctl.BackColor = $cField; $ctl.ForeColor = $cFg; $ctl.Anchor = 'Left'
    if ($ctl -is [System.Windows.Forms.ComboBox]) { $ctl.FlatStyle = 'Flat' } else { $ctl.BorderStyle = 'FixedSingle' }
    $row.Controls.Add($lab, 0, 0); $row.Controls.Add($ctl, 1, 0)
    if ($s.Help) {
        $h = New-Label $s.Help $null $cDim
        $h.MaximumSize = New-Object System.Drawing.Size((S 580), 0)
        $row.Controls.Add($h, 0, 1); $row.SetColumnSpan($h, 2)
    }
    [void]$flow.Controls.Add($row)
    [void]$controls.Add(@{ Def = $s; Ctl = $ctl; Loaded = $cur })
}
$script:loading = $false
$kh = New-Label $keyHelp $null $cDim; $kh.MaximumSize = New-Object System.Drawing.Size((S 580), 0); $kh.Margin = New-Object System.Windows.Forms.Padding(0, (S 10), 0, 0)
[void]$pages['Keyboard'].Controls.Add($kh)

function Get-ControlValue($c) { return ([string]$c.Ctl.Text).Trim() }
function Save-Settings {
    $bad = @()
    foreach ($c in $controls) {
        $v = Get-ControlValue $c
        if ($c.Def.Check -and $v -ne $c.Loaded -and $v -notmatch $c.Def.Check) { $bad += "$($c.Def.Label): '$v'" }
        if ($v -match '[\r\n;]') { $bad += "$($c.Def.Label): no ; or line breaks" }
    }
    if ($bad.Count) {
        [void][System.Windows.Forms.MessageBox]::Show("These values are not valid, nothing was saved:`n`n" + ($bad -join "`n"), 'SSBU launcher', 'OK', 'Warning'); return $false
    }
    $changed = @{ G = 0; C = 0 }
    $files = @{ G = (Read-IniLines $P.Graphics); C = (Read-IniLines $P.Controllers) }      # read again: the F1 menu may have written meanwhile
    foreach ($c in $controls) {
        $v = Get-ControlValue $c
        if ($v -eq $c.Loaded) { continue }
        $files[$c.Def.F] = Set-IniValue $files[$c.Def.F] $c.Def.S $c.Def.K $v
        $changed[$c.Def.F]++; $c.Loaded = $v
    }
    try {
        if ($changed.G) { Write-IniLines $P.Graphics $files.G }
        if ($changed.C) { Write-IniLines $P.Controllers $files.C }
    } catch {
        [void][System.Windows.Forms.MessageBox]::Show("Could not write the settings:`n" + $_.Exception.Message, 'SSBU launcher', 'OK', 'Error'); return $false
    }
    $script:files = $files; $script:dirty = $false; $form.Text = 'SSBU native - launcher'
    $n = $changed.G + $changed.C
    $lblStatus.ForeColor = $cOk
    $lblStatus.Text = $(if ($n) { "Saved $n setting(s). Game settings are used at the next start; controller settings at once." } else { 'Nothing changed.' })
    return $true
}
$btnSave.Add_Click({ [void](Save-Settings) })

# --- Home
$homePg = $pages['Home']
$homeSub = New-Label 'Super Smash Bros. Ultimate as a native PC program. Your own game, built on your PC.' $null $cDim
$homeSub.Margin = New-Object System.Windows.Forms.Padding(0, 0, 0, (S 16)); [void]$homePg.Controls.Add($homeSub)
[void]$homePg.Controls.Add((New-Label 'Status' $fontH2))
$statusBox = New-Object System.Windows.Forms.TableLayoutPanel
$statusBox.ColumnCount = 2; $statusBox.AutoSize = $true; $statusBox.Margin = New-Object System.Windows.Forms.Padding(0, (S 4), 0, (S 16))
[void]$statusBox.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', (S 30))))
[void]$homePg.Controls.Add($statusBox)
[void]$homePg.Controls.Add((New-Label 'Folders and tools' $fontH2))
$tools = New-Object System.Windows.Forms.FlowLayoutPanel
$tools.AutoSize = $true; $tools.WrapContents = $true; $tools.MaximumSize = New-Object System.Drawing.Size((S 780), 0); $tools.Margin = New-Object System.Windows.Forms.Padding(0, (S 4), 0, (S 16))
[void]$homePg.Controls.Add($tools)
foreach ($t in @(@('Saves folder', { Open-Path $P.Saves }), @('Logs folder', { Open-Path $P.Logs }), @('Settings folder', { Open-Path $P.Config }),
                 @('Game folder', { Open-Path $P.Work }), @('Install / repair / update', { Start-Process -FilePath $P.Install -WorkingDirectory $repo }))) {
    $b = New-Button $t[0] 170; $b.Add_Click($t[1]); [void]$tools.Controls.Add($b)
}
$quick = New-Label 'New here? Read the Setup guide. Going competitive? See the Competitive page (unlock fighters and stages, low input latency).' $null $cDim
$quick.MaximumSize = New-Object System.Drawing.Size((S 760), 0); [void]$homePg.Controls.Add($quick)

function Add-Status([string]$state, [string]$text) {
    $mark = New-Label $(switch ($state) { 'ok' { [string][char]0x2713 } 'warn' { '!' } default { [string][char]0x2717 } }) $fontBold $(switch ($state) { 'ok' { $cOk } 'warn' { $cWarn } default { $cFail } })
    $l = New-Label $text; $l.MaximumSize = New-Object System.Drawing.Size((S 720), 0)
    $r = $statusBox.RowCount; $statusBox.RowCount++
    $statusBox.Controls.Add($mark, 0, $r); $statusBox.Controls.Add($l, 1, $r)
}
function Update-Home {
    $statusBox.SuspendLayout(); $statusBox.Controls.Clear(); $statusBox.RowCount = 0
    $installed = (Test-Path -LiteralPath $P.Exe) -and (Test-Path -LiteralPath $P.GameDll)
    if ($installed) {
        $ver = ''
        try { $ver = (Get-Content -LiteralPath $P.State -Raw | ConvertFrom-Json).game_version } catch {}
        Add-Status 'ok' ("The game is installed" + $(if ($ver) { " (game version $ver)" } else { '' }) + " in $($P.Build).")
    } elseif (Test-Path -LiteralPath $P.Exe) {
        Add-Status 'fail' 'The game library (ssbu_game_eh_o1.dll) is missing: usually an antivirus removed it. See Troubleshooting, then Install / repair.'
    } else {
        Add-Status 'fail' 'The game is not installed yet. Read the Setup guide, then press Install / repair / update.'
    }
    $vk = Test-Path -LiteralPath (Join-Path $env:WINDIR 'System32\vulkan-1.dll')
    Add-Status $(if ($vk) { 'ok' } else { 'fail' }) $(if ($vk) { 'Vulkan driver found.' } else { 'No Vulkan driver found: install the newest driver of your GPU (Vulkan 1.3).' })
    if (Test-Path -LiteralPath (Join-Path $P.Saves 'save\save_data')) { Add-Status 'ok' 'Save data found.' } else { Add-Status 'warn' 'No save yet: the game makes one at the first start (Setup guide, step 4, to use your own).' }
    $dlc = 0
    if (Test-Path -LiteralPath $P.Dlc) { $dlc = @(Get-Content -LiteralPath $P.Dlc | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith('#') }).Count }
    Add-Status $(if ($dlc) { 'ok' } else { 'warn' }) $(if ($dlc) { "$dlc downloadable contents listed." } else { 'No DLC listed (optional: put your DLC .nsp files next to the game and run the installer).' })
    $ua = Get-IniValue $script:files.G '' 'unlock_all' 'off'
    Add-Status $(if ($ua -eq 'on') { 'ok' } else { 'warn' }) $(if ($ua -eq 'on') { 'Every fighter and stage is unlocked (Competitive page).' } else { 'Fighters and stages unlock as in the game. Competitive page: Unlock fighters and stages.' })
    if (Get-Process -Name 'ssbu_native' -ErrorAction SilentlyContinue) { Add-Status 'warn' 'The game is running now. Game settings saved here are used at its next start.' }
    $statusBox.ResumeLayout()
    $btnPlay.Enabled = $installed
}

# --- Controls page: the controller test
$ct = New-Button 'Test controllers' 170
$ct.Add_Click({
    if (-not (Test-Path -LiteralPath $P.Exe)) { Open-Path $P.Exe; return }
    Start-Process -FilePath $P.Exe -ArgumentList '--pad-test' -WorkingDirectory $P.Work
})
$co = New-Button 'Open controllers.ini' 170
$co.Add_Click({ Open-Path $P.Controllers })
$ctr = New-Object System.Windows.Forms.FlowLayoutPanel; $ctr.AutoSize = $true; $ctr.Controls.Add($ct); $ctr.Controls.Add($co)
$ctr.Margin = New-Object System.Windows.Forms.Padding(0, (S 6), 0, 0)
[void]$pages['Controls'].Controls.Add($ctr)

# --- the help pages: '# ' = a heading, '- ' = a point
function Add-Text($flow, [string]$text) {
    foreach ($line in ($text -split "`r?`n")) {
        if (-not $line.Trim()) { continue }
        if ($line.StartsWith('# ')) { $l = New-Label $line.Substring(2) $fontH2; $l.Margin = New-Object System.Windows.Forms.Padding(0, (S 14), 0, (S 4)) }
        elseif ($line.StartsWith('- ')) { $l = New-Label ([string][char]0x2022 + '  ' + $line.Substring(2)); $l.Margin = New-Object System.Windows.Forms.Padding((S 12), (S 1), 0, (S 1)) }
        else { $l = New-Label $line }
        $l.MaximumSize = New-Object System.Drawing.Size((S 760), 0)
        [void]$flow.Controls.Add($l)
    }
}
Add-Text $pages['Guide'] $guide
Add-Text $pages['Trouble'] $trouble
Add-Text $pages['About'] $about
$links = New-Object System.Windows.Forms.FlowLayoutPanel; $links.AutoSize = $true; $links.Margin = New-Object System.Windows.Forms.Padding(0, (S 14), 0, 0)
foreach ($t in @(@('Website', 'https://denkishi.github.io/opensmash-site/'), @('Discord', 'https://discord.gg/RyZpx3u2t4'), @('README', (Join-Path $repo 'README.md')), @('Licence', (Join-Path $repo 'LICENSE')))) {
    $b = New-Button $t[0] 120; $b.Tag = $t[1]; $b.Add_Click({ Start-Process -FilePath $this.Tag }); [void]$links.Controls.Add($b)
}
[void]$pages['About'].Controls.Add($links)

# --- Play and closing
$btnPlay.Add_Click({
    if ($script:dirty -and -not (Save-Settings)) { return }
    if (Get-Process -Name 'ssbu_native' -ErrorAction SilentlyContinue) {
        if ([System.Windows.Forms.MessageBox]::Show('The game is already running. Start another copy?', 'SSBU launcher', 'YesNo', 'Question') -ne 'Yes') { return }
    }
    Start-Process -FilePath $P.Play -WorkingDirectory $repo
    $lblStatus.ForeColor = $cOk; $lblStatus.Text = 'Starting the game...'
    if ($chkClose.Checked) { $form.Close() }
})
$form.Add_FormClosing({
    if ($script:dirty -and -not $Shot) {
        $r = [System.Windows.Forms.MessageBox]::Show('Save your changes before closing?', 'SSBU launcher', 'YesNoCancel', 'Question')
        if ($r -eq 'Cancel' -or ($r -eq 'Yes' -and -not (Save-Settings))) { $_.Cancel = $true }
    }
})

if (-not $pages.Contains($Page)) { $Page = 'Home' }
Show-Page $Page
if ($Shot) {      # test only: a picture of every page, then close
    $shotT = New-Object System.Windows.Forms.Timer; $shotT.Interval = 1500
    $shotT.Add_Tick({
        $shotT.Stop()
        if (-not (Test-Path -LiteralPath $Shot)) { New-Item -ItemType Directory -Force -Path $Shot | Out-Null }
        foreach ($k in $pages.Keys) {
            Show-Page $k; $form.Refresh()
            $bm = New-Object System.Drawing.Bitmap($form.Width, $form.Height)
            $form.DrawToBitmap($bm, (New-Object System.Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
            $bm.Save((Join-Path $Shot "$k.png"), [System.Drawing.Imaging.ImageFormat]::Png); $bm.Dispose()
        }
        $form.Close()
    })
    $form.Add_Shown({ $shotT.Start() })
}
[System.Windows.Forms.Application]::Run($form)
