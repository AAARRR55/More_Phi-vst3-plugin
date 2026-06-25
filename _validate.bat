@echo off
REM _validate.bat — configure + compile the changed targets from Phases 1-4
REM in a fresh build-validate/ dir, then run the new tests. Debug + tests ON so
REM the licensing dev-key ship-gate is exempt (we test the rotation infra, not
REM the block itself, here).
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [validate] vcvars failed & exit /b 1 )
cd /d G:\More_Phi-vst3-plugin

if not exist build-validate\CMakeCache.txt (
    echo [validate] configuring build-validate ...
    cmake -B build-validate -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORE_PHI_BUILD_TESTS=ON -Wno-deprecated
    if errorlevel 1 ( echo [validate] CONFIGURE FAILED & exit /b 1 )
)

echo [validate] building MorePhi shared lib ...
cmake --build build-validate --target MorePhi --config Debug
if errorlevel 1 ( echo [validate] MorePhi BUILD FAILED & exit /b 1 )

echo [validate] building test exe ...
cmake --build build-validate --target MorePhiTests --config Debug
if errorlevel 1 ( echo [validate] MorePhiTests BUILD FAILED & exit /b 1 )

echo [validate] running targeted tests ...
cd build-validate
ctest -C Debug -R "Licensing|llm|activation|RestLlm" --output-on-failure
if errorlevel 1 ( echo [validate] TEST RUN FAILED & exit /b 1 )

echo [validate] ALL CHECKPOINT-A CHECKS PASSED
exit /b 0
