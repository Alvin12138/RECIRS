#!/usr/bin/env python3
"""
CFI Instrumentation Script for OpenPLC / matiec generated code
Support for conditional compilation: # ifdef CFI-PROTECT... # endif
Support tag mechanism: Each protected function is assigned a unique tag, and the first execution is synchronized to TEE to establish a golden backup
Support C++overloading: Same name functions distinguish tags through parameter signatures

Usage: python3 cfi_instrument.py POUS.c Config0.c Res0.c
       python3 cfi_instrument.py --all POUS.c Config0.c Res0.c core/*.cpp
"""

import re
import sys
import argparse
import hashlib

# ============================================================================
# Tag Generation with Overload Support
# ============================================================================
# Track (file_path, func_name) -> count to detect overloaded functions
_overload_tracker = {}

def generate_tag(func_name, file_path="", param_sig=""):
    """
    Generate function unique label - use stable hash
    
    For C++overloaded functions: Same name but different parameters 
    ->Automatically include parameter signature to generate different tags
    """
    key = (file_path, func_name)
    _overload_tracker[key] = _overload_tracker.get(key, 0) + 1
    
    # If overloading is detected (multiple occurrences of functions with the same name in the same file), 
    # or parameter signatures are provided,
    # then, include it in the hash to generate a unique tag
    if param_sig:
        unique_str = f"{file_path}:{func_name}:{param_sig}"
    else:
        unique_str = f"{file_path}:{func_name}"
    
    tag = int(hashlib.md5(unique_str.encode()).hexdigest()[:8], 16)
    return tag & 0xFFFFFFFF

def reset_overload_tracker():
    """Reset overload counter before processing new files"""
    _overload_tracker.clear()

# ============================================================================
# Function Body Parsing (captures parameter signatures for overloads)
# ============================================================================

def find_function_bodies(content):
    """
    Find all function bodies (supports nested curly braces)
    return [(func_name, param_sig, start_pos, end_pos, body_text), ...]
    
    param_sig Tag differentiation for C++overloaded functions
    """
    functions = []
    # Capture function name and parameter signature: void name(params) {
    # Match global function (line header) and class method (indentation)
    # Group 2 = func_name, Group 3 = param_sig (e.g., "(const T& x, int y)")
    pattern = r'^(\s*)(?:static\s+|virtual\s+)?(?:\w+[\s&*:]+)+(\w+)\s*(\([^)]*\))\s*\{'

    for match in re.finditer(pattern, content, re.MULTILINE):
        func_name = match.group(2)
        param_sig = match.group(3)  # Parameter signature, such as "(const ControlRelayOutputBlock&)"
        start = match.end() - 1
        brace_count = 0
        end = start
        for i in range(start, len(content)):
            if content[i] == '{':
                brace_count += 1
            elif content[i] == '}':
                brace_count -= 1
                if brace_count == 0:
                    end = i
                    break
        body = content[start:end+1]
        functions.append((func_name, param_sig, start, end, body))

    return functions

def should_instrument(func_name):
    must_protect = ['_body__', '_init__', '_retrieve__', '_publish__', '_run__']
    for suffix in must_protect:
        if suffix in func_name:
            return True, 'matiec_generated'

    optional = ['BOOL_TO_REAL', 'MUL__REAL', 'ADD__REAL', 'DIV__REAL', 'SUB__REAL',
                'GT__REAL', 'LT__REAL', 'EQ__REAL', 'AND__BOOL', 'OR__BOOL',
                'NOT__BOOL', 'MOVE__REAL', 'SEL__REAL', 'LIMIT__REAL']
    for prefix in optional:
        if prefix in func_name:
            return True, 'iec_lib'

    skip = ['__GET_VAR', '__SET_VAR', '__BOOL_LITERAL', '__REAL_LITERAL',
            '__INT_LITERAL', '__STRING_LITERAL', '__TIME_LITERAL',
            'static_inline', 'INLINE', '__end', '__init', '__cleanup',
            'if', 'switch', 'while', 'for', 'catch']
    for s in skip:
        if s in func_name:
            return False, 'skip_inline'

    return True, 'default'

def is_line_in_comment(line, block_comment_active):
    """
    Determine whether the current line is in a comment
    return (is_effective_code, new_block_comment_state)
    is_effective_code=True indicates that this line contains valid code (not pure comments)
    """
    stripped = line.strip()
    
    # blank line
    if not stripped:
        return False, block_comment_active
    
    # Line comments //
    if stripped.startswith('//'):
        return False, block_comment_active
    
    # process /* */ block comments
    if block_comment_active:
        if '*/' in stripped:
            # End block comments, check if there is valid code after */
            after_comment = stripped.split('*/', 1)[1].strip()
            if after_comment and not after_comment.startswith('//'):
                return True, False
            return False, False
        return False, True  # Still in block comments
    
    if '/*' in stripped:
        before_comment = stripped.split('/*', 1)[0].strip()
        if before_comment:
            # /* There was valid code before
            return True, False
        # /* At the beginning, check */ in peers
        if '*/' in stripped:
            after_comment = stripped.split('*/', 1)[1].strip()
            if after_comment and not after_comment.startswith('//'):
                return True, False
            return False, False
        return False, True  # block comments starts
    
    return True, False


def is_effective_return(line, block_comment_active):
    """Determine whether a return statement is valid code (not a return in comments)"""
    stripped = line.strip()
    
    # Check if it is in the block comments
    if block_comment_active:
        if '*/' in stripped:
            after = stripped.split('*/', 1)[1].strip()
            # After the block comments is completed, check if there is a valid return
            if after.startswith('return') or after.startswith('// return'):
                return False  # Simplified processing: The return after */is not docked (rarely seen)
            return False
        return False  # Still in block comments
    
    # Check line comments //
    if stripped.startswith('//'):
        return False
    
    # Check/* on the same line and after return
    if '/*' in stripped:
        before = stripped.split('/*', 1)[0].strip()
        if before.startswith('return') or before == 'return;':
            return True
        return False  # return is a comment after/*
    
    # valid return
    if stripped == 'return;' or stripped.startswith('return '):
        return True
    
    return False


def instrument_body(body, func_name, param_sig, reason, tag):
    """
    Insert CFI_PROLOGUE_TAG and CFI_EPILOGUE_TAG wrapped in # ifdef CFI_PROTECT into the function body
    
    rules:
    1. Do not insert CFI (including//line comments and//* */block comments) in comments code
    2. Do not insert CFI in commented out code
    3. PROLOGUE insert before the first valid line of code
    4. EPILOGUE insert before each valid return; insert before } when there is no return
    """
    lines = body.split('\n')
    new_lines = []
    tag_hex = f"0x{tag:08X}U"
    block_comment = False
    prologue_inserted = False
    has_effective_return = False
    
    # Line 0: opening brace "{"
    new_lines.append(lines[0])
    
    for idx, line in enumerate(lines[1:-1], 1):
        stripped = line.strip()
        
        # Determine the current row status using is_ine_in_comment
        is_code, block_comment = is_line_in_comment(line, block_comment)
        
        if not is_code:
            # Pure comment line or blank line: Keep as is, without inserting a post
            new_lines.append(line)
            continue
        
        # Insert PROLOGUE here (before the first valid line of code)
        if not prologue_inserted:
            indent = len(line) - len(line.lstrip())
            new_lines.append(' ' * indent + '#if CFI_PROTECT')
            new_lines.append(f'{" " * indent}/* CFI: {reason}, tag={tag_hex} */')
            new_lines.append(f'{" " * indent}CFI_PROLOGUE_TAG({tag_hex});')
            new_lines.append(' ' * indent + '#endif')
            new_lines.append('')
            prologue_inserted = True
        
        # Insert EPILOGUE before a valid return statement
        if is_effective_return(line, False):
            has_effective_return = True
            indent = len(line) - len(line.lstrip())
            new_lines.append(' ' * indent + '#if CFI_PROTECT')
            new_lines.append(f'{" " * indent}CFI_EPILOGUE_TAG_PAC({tag_hex});')
            new_lines.append(' ' * indent + '#endif')
        
        new_lines.append(line)
    
    # The last line of the function body:closing brace "}"
    # If there is no valid return, insert EPILOGUE before }
    if not has_effective_return:
        last_indent = 4
        for line in reversed(lines[1:-1]):
            if line.strip():
                last_indent = len(line) - len(line.lstrip())
                break
        new_lines.append(' ' * last_indent + '#if CFI_PROTECT')
        new_lines.append(f'{" " * last_indent}CFI_EPILOGUE_TAG_PAC({tag_hex});')
        new_lines.append(' ' * last_indent + '#endif')
    
    new_lines.append(lines[-1])
    return '\n'.join(new_lines)

# ============================================================================
# Forward-Edge: Comprehensive Indirect Call Instrumentation
# ============================================================================
# Covers: pthread_create, C++ virtual functions, callback assignments,
#         sigaction, and function pointer variable assignments

def find_pthread_create_calls(content):
    """search pthread_create calls, return [(line_num, line_text, thread_func_name), ...]"""
    calls = []
    lines = content.split('\n')
    
    for line_num, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.startswith('//') or stripped.startswith('#'):
            continue
        # match pthread_create(&thread, attr, start_routine, arg)
        # do not match CFI_PTHREAD_CREATE macro (already inserted)
        if 'CFI_PTHREAD_CREATE' in stripped:
            continue
        m = re.search(r'pthread_create\s*\([^,]*,\s*[^,]*,\s*([^,)]+)\s*,', stripped)
        if m:
            func_name = m.group(1).strip()
            func_name = re.sub(r'^&', '', func_name)
            func_name = re.sub(r'\([^)]*\)', '', func_name).strip()
            
            C_KEYWORDS = {'if', 'while', 'for', 'switch', 'case', 'return', 'sizeof'}
            if func_name and func_name not in C_KEYWORDS:
                calls.append((line_num, line, func_name))
    
    return calls

def find_virtual_functions(content):
    """search C++ virtual func，return [(line_num, class_method_name, method_signature), ...]"""
    vfuncs = []
    lines = content.split('\n')
    current_class = None
    
    for line_num, line in enumerate(lines, 1):
        stripped = line.strip()
        
        # detect class definition: class Name: public Base {
        cm = re.search(r'class\s+(\w+)\s*[:{]', stripped)
        if cm and not stripped.endswith(';'):
            current_class = cm.group(1)
            continue
        
        if stripped.startswith('//') or stripped.startswith('#'):
            continue
        
        # detect virtual func: virtual ReturnType MethodName(params) {
        vm = re.search(r'virtual\s+(?:\w+[&\s*:]+)+\s+(\w+)\s*\([^)]*\)\s*\{', stripped)
        if vm and current_class:
            method_name = vm.group(1)
            full_name = f"{current_class}::{method_name}"
            vfuncs.append((line_num, full_name, method_name))
    
    return vfuncs

def find_callback_assignments(content):
    """Search for callback function pointer assignment, return [(line_num, line_text, func_ptr_var, target_func), ...]"""
    callbacks = []
    lines = content.split('\n')
    
    # mode: TypeName callback_var = function_name;
    # or: callback_var = function_name;
    for line_num, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.startswith('//') or stripped.startswith('#'):
            continue
        
        # match: _callback var_name = func_name;
        cm = re.search(r'\w*_callback\s+(\w+)\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;', stripped)
        if cm:
            var_name = cm.group(1)
            func_name = cm.group(2)
            if func_name not in {'NULL', 'nullptr', '0'}:
                callbacks.append((line_num, line, var_name, func_name))
    
    return callbacks

def instrument_forward_edge(content, filename):
    """Main function for forward edge insertion: pthread_create+virtual function+callback assignment"""
    stats = {'pthread_create': 0, 'virtual': 0, 'callback': 0}
    
    # --- 1. pthread_create ---
    pth_calls = find_pthread_create_calls(content)
    lines = content.split('\n')
    
    for call_info in pth_calls:
        line_num, line, func_name = call_info
        idx = line_num - 1
        tag = generate_tag(func_name, filename)
        indent = len(line) - len(line.lstrip())
        reg_lines = [
            f"{' '*indent}#if CFI_PROTECT",
            f"{' '*indent}/* CFI FWD: pthread_create -> '{func_name}' */",
            f"{' '*indent}cfi_register_target((uint64_t)(uintptr_t){func_name}, 0x{tag:08X}U);",
            f"{' '*indent}#endif",
            lines[idx]
        ]
        lines[idx] = '\n'.join(reg_lines)
        stats['pthread_create'] += 1
    
    # --- 2. C++ vitual func ---
    vfuncs = find_virtual_functions(content)
    for line_num, full_name, method_name in vfuncs:
        idx = line_num - 1
        # In the body of the function opening brace, insert the first line after CFI_VCALL
        # Need to find the location of {
        line = lines[idx]
        # find opening brace
        brace_pos = line.find('{')
        if brace_pos < 0:
            continue
        
        vlabel = generate_tag(full_name, filename)
        indent = len(line) - len(line.lstrip()) + 4
        vcall_code = (
            f"#if CFI_PROTECT\n"
            f"{' '*indent}/* CFI FWD: Virtual call guard for '{full_name}' */\n"
            f"{' '*indent}CFI_VCALL(this, {method_name});\n"
            f"{' '*indent}#endif\n"
        )
        # insert after {
        lines[idx] = line[:brace_pos+1] + '\n' + vcall_code + line[brace_pos+1:]
        stats['virtual'] += 1
    
    # --- 3. Callback function pointer assignment ---
    callbacks = find_callback_assignments(content)
    for line_num, line, var_name, func_name in callbacks:
        idx = line_num - 1
        tag = generate_tag(func_name, filename)
        indent = len(line) - len(line.lstrip())
        cb_lines = [
            f"{' '*indent}#if CFI_PROTECT",
            f"{' '*indent}/* CFI FWD: Callback assignment '{var_name}' = '{func_name}' */",
            f"{' '*indent}cfi_register_target((uint64_t)(uintptr_t){func_name}, 0x{tag:08X}U);",
            f"{' '*indent}#endif",
            lines[idx]
        ]
        lines[idx] = '\n'.join(cb_lines)
        stats['callback'] += 1
    
    return '\n'.join(lines), stats

def instrument_file(filename, args):
    with open(filename, 'r') as f:
        original = f.read()

    reset_overload_tracker()
    functions = find_function_bodies(original)
    print(f"[CFI-INSTRUMENT] {filename}: found {len(functions)} functions")

    # Detect and report overloaded functions
    from collections import Counter
    name_counts = Counter(fn for fn, ps, s, e, b in functions)
    overloads = {k: v for k, v in name_counts.items() if v > 1}
    if overloads:
        print(f"  [OVERLOADS] Detected overloaded functions:")
        for fname, count in overloads.items():
            sigs = [ps for fn, ps, s, e, b in functions if fn == fname]
            print(f"    {fname}: {count} overloads")
            for sig in sigs:
                print(f"      {sig}")

    modified = original
    stats = {'protected': 0, 'skipped': 0, 'reasons': {}, 'forward': {}}

    # Instrumentation towards the backward-edge
    for func_name, param_sig, start, end, body in reversed(functions):
        # Skip functions that have already been inserted (to prevent duplicate insertion)
        if 'CFI_PROLOGUE_TAG_PAC' in body or 'CFI_EPILOGUE_TAG_PAC' in body:
            stats['skipped'] += 1
            print(f"  [=] {func_name}{param_sig} (already instrumented)")
            continue
        
        should, reason = should_instrument(func_name)

        if should:
            tag = generate_tag(func_name, filename, param_sig)
            new_body = instrument_body(body, func_name, param_sig, reason, tag)
            modified = modified[:start] + new_body + modified[end+1:]
            stats['protected'] += 1
            stats['reasons'][reason] = stats['reasons'].get(reason, 0) + 1
            print(f"  [+] {func_name}{param_sig} ({reason}, tag=0x{tag:08X})")
        else:
            stats['skipped'] += 1
            print(f"  [-] {func_name}{param_sig} ({reason})")

    # Forward edge pile insertion
    if args.forward or args.all:
        modified, fwd_stats = instrument_forward_edge(modified, filename)
        stats['forward'] = fwd_stats
        if fwd_stats.get('pthread_create', 0) > 0:
            print(f"  [FWD] {fwd_stats['pthread_create']} pthread_create call(s) instrumented")

    if not args.dry_run and modified != original:
        with open(filename, 'w') as f:
            f.write(modified)

    print(f"\nSummary: protected={stats['protected']}, skipped={stats['skipped']}")
    print(f"Reasons: {stats['reasons']}")
    if stats.get('forward'):
        print(f"Forward-edge: {stats['forward']}")
    return stats

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='CFI Instrumentation for OpenPLC with Tags')
    parser.add_argument('files', nargs='+', help='C files to instrument')
    parser.add_argument('--dry-run', action='store_true', help='Show what would be done')
    parser.add_argument('--forward', action='store_true', help='Enable forward-edge (pthread_create only)')
    parser.add_argument('--all', action='store_true', help='Enable backward + forward edge')
    args = parser.parse_args()

    if args.all:
        args.forward = True

    for fname in args.files:
        print(f"\n{'='*60}")
        instrument_file(fname, args)
