# scripts

Local dev tooling.

| Script           | What it does                                                         |
| ---------------- | ------------------------------------------------------------------- |
| `dev.sh`         | Build core (Release) + start backend `:8000` + frontend `:5173`.    |
| `build-core.sh`  | Configure + build the C++ core; prints `nabla_solve --version`.     |

```bash
scripts/dev.sh                 # the one-command local stack
scripts/build-core.sh Debug    # build the core in a different config
```

Prerequisites: a C++20 compiler, CMake ≥ 3.25, [uv](https://docs.astral.sh/uv/),
and Node.js ≥ 18.
