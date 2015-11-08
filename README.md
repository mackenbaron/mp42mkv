# mp42mkv

## env
 - Visual Studio 2013 +
 - gcc (Non full tested)

## codecs
 - Audio: AAC\MP3\AC3\DTS\ALAC
 - Video: H264\HEVC\MPEG4(XVID)
 - Misc: Chapters, Metadata and Cover.

## execute
 - mp42mkv input.mp4 output.mkv
 - mp42mkv input.mp4 output.mkv clip-start-seconds clip-range-time-seconds

## samples
 - mp42mkv "C:\test.m4a" "C:\test.mka"
 - mp42mkv "C:\test.mp4" "C:\test.mkv"
 - mp42mkv "C:\test.mp4" "C:\test.mkv" 30
 - mp42mkv "C:\test.mp4" "C:\test.mkv" 30 90
 - mp42mkv "C:\test.mp4" "C:\test.mkv" 0 30