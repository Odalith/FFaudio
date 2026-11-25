#!/bin/bash
# Check the code that runs on the audio callback (between RT-BEGIN and RT-END markers in lib/) against the real-time
# rules of docs/threading.md: no locks to wait on, no allocation, no logging, no FFmpeg, no application callbacks.
# SDL_TryLockMutex, SDL_UnlockMutex and SDL_SemPost are allowed (they never block). Exit status 1 on a finding.
src=$(cd "$(dirname "$0")/../.." && pwd)
banned='SDL_LockMutex|SDL_CondWait|SDL_SemWait|SDL_Delay|SDL_CreateThread|SDL_WaitThread|SDL_LockAudioDevice'
banned+='|av_malloc|av_mallocz|av_calloc|av_realloc|av_free|av_freep|av_strdup|av_asprintf'
banned+='|[^_a-z]malloc[(]|[^_a-z]calloc[(]|[^_a-z]realloc[(]|[^_a-z]free[(]|strdup[(]'
banned+='|ff_log|av_log|printf[(]|fprintf[(]|puts[(]'
banned+='|avcodec_|avformat_|avfilter_|av_buffersrc|av_buffersink|swr_'
banned+='|->custom[(]'
status=0
regions=0
for f in "$src"/lib/*.c; do
    out=$(awk -v banned="$banned" -v file="${f#$src/}" '
        /RT-BEGIN/ { inside = 1; regions++; next }
        /RT-END/   { inside = 0; next }
        inside {
            code = $0
            sub(/\/\/.*/, "", code)                      # comments may name anything
            if (code ~ banned) printf "%s:%d: %s\n", file, NR, $0
        }
        END { if (inside) printf "%s: RT-BEGIN without RT-END\n", file; printf "#%d\n", regions }' "$f")
    n=$(echo "$out" | sed -n 's/^#//p')
    regions=$((regions + n))
    findings=$(echo "$out" | grep -v '^#' || true)
    if [ -n "$findings" ]; then echo "$findings"; status=1; fi
done
[ "$regions" -gt 0 ] || { echo "rt_lint: no RT-BEGIN regions found" >&2; status=1; }
[ $status -eq 0 ] && echo "rt_lint: $regions real-time regions clean"
exit $status
