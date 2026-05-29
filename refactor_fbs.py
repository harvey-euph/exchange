import os
import re

def refactor_enum_file(file_path):
    with open(file_path, 'r') as f:
        content = f.read()
    
    # Match export enum Name { ... }
    match = re.search(r'export enum (\w+) \{(.*?)\}', content, re.DOTALL)
    if not match:
        # Check if already refactored but maybe with wrong syntax
        match = re.search(r'export const (\w+) = \{(.*?)\} as const;', content, re.DOTALL)
        if not match:
            return

    enum_name = match.group(1)
    enum_body = match.group(2)
    
    # Replace = with : in the body
    new_body = enum_body.replace('=', ':')
    
    new_content = f"export const {enum_name} = {{{new_body}}} as const;\n"
    new_content += f"export type {enum_name} = (typeof {enum_name})[keyof typeof {enum_name}];\n"
    
    # Find the start of the definition (either enum or our failed const)
    start_match = re.search(r'(export enum|export const) ' + enum_name, content)
    # Find the end of the original definition or the previous failed attempt
    # For enum it ends at }, for our failed attempt it ended at typeof {enum_name}];
    end_pattern = rf'typeof {enum_name}\]\[keyof typeof {enum_name}\];'
    end_match = re.search(end_pattern, content)
    
    if end_match:
        updated_content = content[:start_match.start()] + new_content + content[end_match.end():]
    else:
        # Fallback to just replacing the enum match if it was first time
        updated_content = content[:match.start()] + new_content + content[match.end():]
    
    with open(file_path, 'w') as f:
        f.write(updated_content)
    print(f"Refactored enum in {file_path}")

def add_casts_to_table_file(file_path):
    with open(file_path, 'r') as f:
        content = f.read()
    
    enums = ['OrderAction', 'RejectCode', 'OrderType', 'Side', 'ExecType']
    
    updated_content = content
    for enum in enums:
        # Ensure we don't double cast if we run it again
        # Match pattern: return offset ? this.bb!.readInt8(this.bb_pos + offset) : OrderAction.New;
        # But NOT: return offset ? this.bb!.readInt8(this.bb_pos + offset) as OrderAction : OrderAction.New;
        
        # This regex looks for the read call, followed by NOT "as Enum", followed by " : Enum.Default"
        # Using a simpler approach: replace any existing cast if it exists or add it.
        pattern = rf'return offset \? (this\.bb!\.readInt\d+\(this\.bb_pos \+ offset\))( as {enum})? : {enum}\.'
        replacement = rf'return offset ? \1 as {enum} : {enum}.'
        updated_content = re.sub(pattern, replacement, updated_content)

    if updated_content != content:
        with open(file_path, 'w') as f:
            f.write(updated_content)
        print(f"Added casts in {file_path}")

fbs_dir = '/home/harvey/exchange-web/src/fbs/exchange'
enum_files = ['order-action.ts', 'reject-code.ts', 'order-type.ts', 'side.ts', 'exec-type.ts']
table_files = ['order-request.ts', 'order-response.ts', 'l2-update.ts', 'l3-update.ts']

for f in enum_files:
    refactor_enum_file(os.path.join(fbs_dir, f))

for f in table_files:
    add_casts_to_table_file(os.path.join(fbs_dir, f))
