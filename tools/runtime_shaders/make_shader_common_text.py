#!/usr/bin/env python3


import sys
from pathlib import Path

DELIMITER = 'BO2SC'


def main():
    source, target = Path(sys.argv[1]), Path(sys.argv[2])
    text = source.read_text(encoding='utf-8')
    if ')' + DELIMITER + '"' in text:
        raise SystemExit('shader_common.h contains the raw string delimiter')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text('// Generated from ' + source.name + ' (do not edit).\n#pragma once\n'
                      'inline constexpr char kShaderCommonText[] = R"' + DELIMITER + '(' + text + ')'
                      + DELIMITER + '";\n', encoding='utf-8')


if __name__ == '__main__':
    main()
