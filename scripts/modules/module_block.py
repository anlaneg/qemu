#!/usr/bin/env python3
# encoding=utf-8
#
# Module information generator
#
# Copyright Red Hat, Inc. 2015 - 2016
#
# Authors:
#  Marc Mari <markmb@redhat.com>
#
# This work is licensed under the terms of the GNU GPL, version 2.
# See the COPYING file in the top-level directory.
#依据/block文件夹中的每个.c文件，生成一条block_driver_modules记录

import sys
import os

#返回变量的值
def get_string_struct(line):
    data = line.split()

    # data[0] -> struct element name
    # data[1] -> =
    # data[2] -> value

    return data[2].replace('"', '')[:-1]

def add_module(fheader, library, format_name, protocol_name):
    lines = []
    #构造成员的值
    lines.append('.library_name = "' + library + '",')
    if format_name != "":
        lines.append('.format_name = "' + format_name + '",')
    if protocol_name != "":
        lines.append('.protocol_name = "' + protocol_name + '",')

    text = '\n        '.join(lines)
    #向文件写入结构体设置
    fheader.write('\n    {\n        ' + text + '\n    },')

def process_file(fheader, filename):
    # This parser assumes the coding style rules are being followed
    with open(filename, "r") as cfile:
        found_start = False
        library, _ = os.path.splitext(os.path.basename(filename))
        #遍历c文件的每一行
        #1。先找到"static BlockDriver"这一行,例如文件block/qcow.c文件
        #   中存在static BlockDriver bdrv_qcow = {
        #2。
        for line in cfile:
            if found_start:
                line = line.replace('\n', '')
                #找到format_name成员赋值行，取format_name
                if line.find(".format_name") != -1:
                    format_name = get_string_struct(line)
                elif line.find(".protocol_name") != -1:
                    #取protocol_name成员设置的值
                    protocol_name = get_string_struct(line)
                elif line == "};":
                    #结构体完成，添加模块
                    add_module(fheader, library, format_name, protocol_name)
                    found_start = False
            elif line.find("static BlockDriver") != -1:
                # 1.找到解析起始行
                found_start = True
                format_name = ""
                protocol_name = ""

#生成文件头
def print_top(fheader):
    fheader.write('''/* AUTOMATICALLY GENERATED, DO NOT MODIFY */
/*
 * QEMU Block Module Infrastructure
 *
 * Authors:
 *  Marc Mari       <markmb@redhat.com>
 */

''')

    fheader.write('''#ifndef QEMU_MODULE_BLOCK_H
#define QEMU_MODULE_BLOCK_H

static const struct {
    const char *format_name;
    const char *protocol_name;
    const char *library_name;
} block_driver_modules[] = {''')

def print_bottom(fheader):
    fheader.write('''
};

#endif
''')

if __name__ == '__main__':
    # First argument: output file
    # All other arguments: modules source files (.c)
    output_file = sys.argv[1]
    with open(output_file, 'w') as fheader:
    	#生成文件头
        print_top(fheader)

    	#argv[1]为输出文件，argv[2:]为输入文件
        for filename in sys.argv[2:]:
            if os.path.isfile(filename):
            	#输入文件必须存在，解析输入文件
                process_file(fheader, filename)
            else:
                print("File " + filename + " does not exist.", file=sys.stderr)
                sys.exit(1)

    	#显示生成文件尾部
        print_bottom(fheader)

    sys.exit(0)
