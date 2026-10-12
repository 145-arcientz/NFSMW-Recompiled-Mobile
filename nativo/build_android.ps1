$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$wrapper = Join-Path $repoRoot 'android\gradlew.bat'

if (-not $env:JAVA_HOME) {
    $studioJbr = Join-Path $env:ProgramFiles 'Android\Android Studio\jbr'
    if (Test-Path -LiteralPath (Join-Path $studioJbr 'bin\java.exe')) {
        $env:JAVA_HOME = $studioJbr
    }
}
if (-not (Get-Command java -ErrorAction SilentlyContinue)) {
    throw 'Java 17 or newer is required. Install a JDK and ensure java is on PATH.'
}
if (-not (Test-Path -LiteralPath $wrapper)) {
    throw 'Gradle wrapper is missing from android\gradlew.bat.'
}
if (-not $env:ANDROID_HOME) {
    $sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
    if (Test-Path -LiteralPath $sdk) { $env:ANDROID_HOME = $sdk }
}
if ($env:ANDROID_HOME) {
    $env:ANDROID_SDK_ROOT = $env:ANDROID_HOME
}

Push-Location (Join-Path $repoRoot 'android')
try {
    # Gradle and javac print warnings on stderr; with 'Stop' PowerShell would abort on the first one.
    $ErrorActionPreference = 'Continue'
    & $wrapper assembleRelease
    if ($LASTEXITCODE -ne 0) { throw "Gradle build failed with exit code $LASTEXITCODE" }
    $apk = Join-Path $repoRoot 'android\app\build\outputs\apk\release\app-release.apk'
    Write-Host "APK: $apk"
} finally {
    Pop-Location
}
