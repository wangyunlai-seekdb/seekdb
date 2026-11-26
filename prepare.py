import pathlib
import re
import subprocess
import sys

repo = pathlib.Path(sys.argv[1])
output = pathlib.Path(sys.argv[2])
output.mkdir(parents=True, exist_ok=True)


def read(path):
    return subprocess.check_output(['git', '-C', str(repo), 'show', 'HEAD:' + path], text=True)


def between(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def write(name, content):
    (output / name).write_text(content, encoding='utf-8')


lib = 'src/oblib/lib/'
header = read(lib + 'utility/ob_smart_call.h')
allocator_header = read(lib + 'thread/protected_stack_allocator.h')
constants = read(lib + 'ob_errno.h')
constants = '\n'.join(re.search(r'^constexpr int ' + name + r' = -?\d+;', constants, re.M)[0] for name in (
    'OB_SUCCESS', 'OB_ERROR', 'OB_INVALID_ARGUMENT', 'OB_ALLOCATE_MEMORY_FAILED', 'OB_ERR_UNEXPECTED', 'OB_SIZE_OVERFLOW'))
macros = '\n'.join(re.search(r'^#define ' + name + r'\([^\n]+', read(lib + 'utility/ob_macro_utils.h'), re.M)[0] for name in ('OB_SUCC', 'OB_FAIL'))
reserve = re.search(r'^static const uint32_t ACHUNK_PRESERVE_SIZE.*;', read(lib + 'utility/alloc_assist.h'), re.M)[0]
write('stack.h', '#pragma once\n#include "shim.h"\n' + macros + '\n' + allocator_header + '\nnamespace oceanbase { namespace common {\n' + constants + '\n' + reserve + '\nint get_stackattr(void *&, size_t &);\nvoid set_stackattr(void *, size_t);\nint check_stack_overflow(bool &, int64_t, int64_t * = nullptr);\n}\n}\n' + '\n'.join(line for line in header.splitlines() if not line.startswith('#include')) + '\n')
common = between(read(lib + 'utility/ob_common_utility.cpp'), '_RLOCAL(char*, g_stackaddr);', 'ObFatalErrExtraInfoGuard::ObFatalErrExtraInfoGuard')
jump = between(read(lib + 'utility/ob_smart_call.cpp'), '#if defined(__x86_64__) && defined(_WIN32)', '} // namespace common')
allocator = read(lib + 'thread/protected_stack_allocator.cpp')
allocator = '\n'.join(line for line in allocator.splitlines() if not line.startswith('#include "'))
write('bounds.cpp', '#include "stack.h"\nnamespace oceanbase::common {\n' + common + '}\n')
write('jump.cpp', '#include "stack.h"\nnamespace oceanbase::common {\n' + jump + '}\n')
write('allocator.cpp', '#include "stack.h"\n' + allocator + '\n')
print(subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip())
