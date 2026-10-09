@echo off
setlocal
set "TASK_TOON_PYTHON=C:\Users\Administrator\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
if not exist "%TASK_TOON_PYTHON%" (
    echo Python runtime not found. Run tools/review_toon_host.py with your Python interpreter.
    exit /b 1
)
"%TASK_TOON_PYTHON%" -X utf8 "%~dp0review_toon_host.py" %*
