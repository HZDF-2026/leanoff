@echo off
echo %~1:1:0: error: stub elaboration failure
echo %~1:2:0: warning: stub warning
echo %~1:3:0: info: declaration uses 'sorry'
exit /b 1
