# AI_USAGE.md

- Tool: Claude (Anthropic chat assistant).
- What it did: produced the first draft of the rule-conflict analysis (DECISIONS.md), the C++ engine, the reference
  model, the rule/model/stress tests, the mutation script and the docs, from the assignment PDF.
- Verification: Claude ran make test, make mutants and ThreadSanitizer in its own environment (all passed).
  I did NOT run the tests on my machine before the deadline: my g++ was too old (error: std::mutex missing)
  and the compiler install was not finished in time.
- What I did myself: chose the SDE assignment, set up the repo and commits, read the engine and decisions, and will run
  the tests locally with the new compiler and study the code before the follow-up discussion.