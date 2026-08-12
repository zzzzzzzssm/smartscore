param(
    [Parameter(Mandatory = $true)]
    [string]$DeviceBaseUrl,

    [Parameter(Mandatory = $true)]
    [string]$JpegPath
)

$ErrorActionPreference = 'Stop'
$resolvedJpeg = (Resolve-Path -LiteralPath $JpegPath).Path
$file = Get-Item -LiteralPath $resolvedJpeg
if ($file.Length -le 0) {
    throw 'JPEG file is empty.'
}

$uri = "$($DeviceBaseUrl.TrimEnd('/'))/api/ai/sheet_to_score"
try {
    $result = Invoke-RestMethod -Uri $uri -Method Post -InFile $resolvedJpeg `
        -ContentType 'image/jpeg' -TimeoutSec 360
} catch {
    $status = $null
    $code = 'request_failed'
    $message = $_.Exception.Message
    if ($_.Exception.Response) {
        $status = [int]$_.Exception.Response.StatusCode
        try {
            $stream = $_.Exception.Response.GetResponseStream()
            $reader = New-Object System.IO.StreamReader($stream)
            $errorBody = $reader.ReadToEnd() | ConvertFrom-Json
            if ($errorBody.error) { $code = [string]$errorBody.error }
            if ($errorBody.message) { $message = [string]$errorBody.message }
        } catch {
            # Keep the transport error without printing an arbitrary body.
        }
    }
    [pscustomobject]@{
        ok = $false
        http_status = $status
        error = $code
        message = $message
        jpeg_bytes = $file.Length
    } | Format-List
    exit 1
}

[pscustomobject]@{
    ok = [bool]$result.ok
    task_id = [string]$result.task_id
    model = [string]$result.model
    kind = [string]$result.kind
    jpeg_bytes = [int64]$result.image_bytes
    dimensions = "$($result.image_width)x$($result.image_height)"
    request_elapsed_ms = [int64]$result.request_elapsed_ms
    cloud_http_status = [int]$result.http_status
    attempts = [int]$result.attempt_count
    input_tokens = [int64]$result.usage.input_tokens
    output_tokens = [int64]$result.usage.output_tokens
    event_count = [int64]$result.event_count
    playback_ready = [bool]$result.playback_ready
    playback_notes = [int64]$result.note_count
} | Format-List
