/* Lives in a library the program loads while it is already running, so that a
   name observed here matches nothing when the plan is resolved and resolves
   later, when the library loads. */
int plugin_step(int n) { return n * 3; }
