@echo off
cd /d "%~dp0"
echo Running Hybrid Core verification suite...
bin-windows\hc_tests.exe > test_results.txt 2>&1
type test_results.txt
echo.
echo Results saved to test_results.txt
pause
