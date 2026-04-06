Build the project and run all tests. Report results concisely.

```bash
cmake --build build 2>&1 | tail -20
ctest --test-dir build --output-on-failure 2>&1 | tail -30
```

If the build fails, show the error and suggest a fix.
If tests fail, show which ones and the failure output.
If everything passes, just say how many tests passed.
