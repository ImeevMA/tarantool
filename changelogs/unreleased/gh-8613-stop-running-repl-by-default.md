## feature/cli

* **[Behavior change]** Disabled starting the Lua REPL by default when running
  Tarantool. Now, Tarantool yields the message that shows the command usage.
  To run Lua REPL just set `-i` flag. To pass Lua script contents via stdin,
  use dash (-) as a SCRIPT name. For more info see help message by running
  `tarantool -h` (gh-8613).
