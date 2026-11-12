# KIRA Instruction Language (KIL) — design v0.1

## Purpose

KIL is the human- and AI-authored systems language planned for KIRA OS. It must express executable program logic in a compact, inspectable form while making resource use and operating-system permissions explicit.

KIL is not natural-language text executed directly, and it is not a shell-command wrapper. Human/AI intent is recorded as metadata; only the parsed, type-checked program is executable.

## Non-negotiable design rules

1. **One compiler pipeline:** KIL lowers into the existing KCC typed IR. It must not introduce a second optimizer or a separate machine-code backend.
2. **Deterministic meaning:** the same source, target and compiler version produce the same typed IR and equivalent program behavior.
3. **Explicit effects:** filesystem, IPC, network, graphics, audio and device access are declared in the source and still require a trusted application manifest grant. A source declaration requests a capability; it never grants one.
4. **Safe defaults:** immutable bindings by default, checked array/slice bounds, explicit mutability, no implicit pointer arithmetic, no implicit shell or dynamic evaluation.
5. **Auditable escape hatches:** raw pointers, inline assembly and unchecked operations require an explicit unsafe block and a target-specific review.
6. **Human readability:** Russian comments and UTF-8 intent strings are allowed; v1 keywords and identifiers remain ASCII for stable tokenization and diagnostics.

## Two language layers: task plan and executable core

KIL должен быть понятен человеку и пригоден для планирования ИИ. Поэтому спецификация разделяет два слоя:

1. **Task layer** — декларативное задание: цель, типизированные входы/выходы, ограничения, шаги, критерии приёмки. Оно компилируется в DAG задач, а не исполняется как свободный текст.
2. **Code layer** — обычные функции KIL, которые реализуют конкретные вычисления. Узлы Task layer могут ссылаться только на типизированные операции/функции из доверенного реестра.

Пример Task layer (контракт синтаксиса; операции пока не заявлены как реализованные API):

```kil
task adapt_driver {
    goal "Подготовить законно распространяемый драйвер для целевой платформы";

    input source: DriverSource;
    input target: TargetTriple;
    output package: KdrvPackage;

    constraints {
        license = "redistributable";
        network = "allowlisted";
        max_memory = 256 * MiB;
        require_signature = true;
    }

    steps {
        inspect(source) -> metadata;
        verify_license(source, metadata) -> license_report;
        adapt(source, target) -> candidate;
        test(candidate, target) -> test_report;
        package(candidate, license_report, test_report) -> package;
    }

    accept {
        require(license_report.redistributable);
        require(test_report.all_required_passed);
        require(package.signature_valid);
    }
}
```

Task layer не может содержать произвольные shell-команды, динамическое выполнение строк или незарегистрированные операции. Каждая операция имеет объявленные типы, эффекты и capability-потребности. Планировщик строит зависимости по входам/выходам, выдаёт каждой задаче собственный лимит памяти/времени и исполняет её в изоляции. AI может предложить план, но не может сам выдать себе разрешения; подпись/подтверждение для опасных действий остаются внешней политикой.

Критерии приёмки — проверяемые выражения или именованные тесты. Естественно-языковое описание цели помогает человеку и модели, но не считается доказательством корректности.

## Proposed source form

```kil
module telemetry;

intent "Считать число событий и вывести итог";

requires {
    fs.read("/data/telemetry/events.bin");
    fs.write("/data/telemetry/summary.txt");
}

fn count_events(data: slice<u8>) -> u64 {
    let mut count: u64 = 0;
    for byte in data {
        if byte == 1 {
            count = count + 1;
        }
    }
    return count;
}

fn main() -> i32 {
    let events: bytes = fs.read_all("/data/telemetry/events.bin");
    let total: u64 = count_events(events);
    fs.write_text("/data/telemetry/summary.txt", total.to_string());
    return 0;
}
```

The example is illustrative syntax, not a claim that the standard library calls already exist. The v1 library surface must be limited to APIs that have real KIRA implementations.

## Core grammar (EBNF draft)

```ebnf
module       = "module", identifier, ";", { item } ;
item         = intent | requires | import | function | task_decl ;
intent       = "intent", string, ";" ;
requires     = "requires", "{", { capability, ";" }, "}" ;
capability   = capability_name, "(", [ argument_list ], ")" ;
import       = "import", string, ";" ;
task_decl    = "task", identifier, "{", goal, { input_decl }, { output_decl },
               constraints, steps, accept, "}" ;
goal         = "goal", string, ";" ;
input_decl   = "input", identifier, ":", type, ";" ;
output_decl  = "output", identifier, ":", type, ";" ;
constraints  = "constraints", "{", { identifier, "=", expression, ";" }, "}" ;
steps        = "steps", "{", { operation, "(", [ argument_list ], ")",
               "->", identifier, ";" }, "}" ;
accept       = "accept", "{", { "require", "(", expression, ")", ";" }, "}" ;
operation    = identifier ;
function     = "fn", identifier, "(", [ parameters ], ")", [ "->", type ], block ;
parameters   = parameter, { ",", parameter } ;
parameter    = identifier, ":", type ;
type         = scalar_type | "bytes" | "str" | "ptr", "<", type, ">"
             | "array", "<", type, ",", integer, ">"
             | "slice", "<", type, ">" ;
scalar_type  = "bool" | "i8" | "i16" | "i32" | "i64"
             | "u8" | "u16" | "u32" | "u64" | "f32" | "f64" ;
block        = "{", { statement }, "}" ;
statement    = binding | assignment | conditional | loop | return_stmt
             | expression, ";" | "unsafe", block ;
binding      = ("let" | "var"), identifier, [ ":", type ], "=", expression, ";" ;
assignment   = expression, assignment_op, expression, ";" ;
conditional  = "if", expression, block, [ "else", block ] ;
loop         = "while", expression, block
             | "for", identifier, "in", expression, block ;
return_stmt  = "return", [ expression ], ";" ;
```

This grammar intentionally leaves operator precedence, generic constraints, error propagation and module visibility to the typed-language specification before parser implementation.

## Type and execution semantics

- Integer types have fixed widths. Overflow is checked by default; wrapping arithmetic is explicit.
- Floating-point operations follow IEEE-754 behavior; unsafe fast-math transformations are not enabled implicitly.
- Arrays have compile-time lengths. Slices carry a length and are bounds checked.
- `let` is immutable; `var` is mutable.
- Pointer types are not implicitly convertible to integers. Pointer arithmetic and direct hardware access require `unsafe`.
- Function effects are the union of effects of called functions and must fit the module's declared capability requests.
- Runtime resource limits (memory, CPU time, handles) are supplied by the application manifest and enforced by the kernel, not trusted from source annotations.

## AI-facing metadata

The optional `intent` string explains the desired result but has no executable semantics. Future checked annotations may include preconditions, postconditions and invariants. The compiler must never treat an AI-generated claim as proof: contracts are accepted only when represented by checkable expressions or tests.

## Integration gate

The existing builder plan places KIL frontend integration after compiler milestones F1–F4. That dependency is retained:

- F1: internal calling convention and call lowering;
- F2: verified loop transformations;
- F3: typed IR facts and value propagation;
- F4: equality-saturation/e-graph optimization.

Until those milestones pass their regression suites, this branch defines the language contract only. No parallel IR, fake backend or placeholder runtime is introduced.

## Required conformance tests before implementation is accepted

1. Lexer: UTF-8 comments/strings, invalid escapes, malformed numeric literals and source locations.
2. Parser: every grammar production, missing delimiters, malformed declarations and recovery after an error.
3. Type checker: signedness, widths, casts, immutable assignment, function signatures and return paths.
4. Safety: bounds failures, checked overflow, unsafe-block requirements and pointer conversion rejection.
5. Effects: undeclared capability use rejected; a source request does not bypass the signed/trusted app manifest.
6. IR equivalence: equivalent KIL and C programs lower to equivalent typed IR for the shared subset.
7. Target parity: the same KIL test corpus passes on x86-64 and ARM64 with equivalent observable results.
