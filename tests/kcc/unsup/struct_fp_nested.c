/* Nested aggregates and arrays must not bypass the FP ABI safety check. */
typedef struct { double lanes[2]; } Pair;
typedef struct { int tag; Pair payload; } Envelope;
static Envelope pass(Envelope v) { return v; }
int main(void) { Envelope v = { 1, {{2.0, 3.0}} }; return (int)pass(v).payload.lanes[0]; }
