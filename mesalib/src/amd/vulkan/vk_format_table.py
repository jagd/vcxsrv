
CopyRight = '''
/**************************************************************************
 *
 * Copyright 2010 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
'''


import sys

from vk_format_parse import *

def layout_map(layout):
    return 'VK_FORMAT_LAYOUT_' + str(layout).upper()


def colorspace_map(colorspace):
    return 'VK_FORMAT_COLORSPACE_' + str(colorspace).upper()


colorspace_channels_map = {
    'rgb': ['r', 'g', 'b', 'a'],
    'srgb': ['sr', 'sg', 'sb', 'a'],
    'zs': ['z', 's'],
    'yuv': ['y', 'u', 'v'],
}


type_map = {
    VOID:     "VK_FORMAT_TYPE_VOID",
    UNSIGNED: "VK_FORMAT_TYPE_UNSIGNED",
    SIGNED:   "VK_FORMAT_TYPE_SIGNED",
    FIXED:    "VK_FORMAT_TYPE_FIXED",
    FLOAT:    "VK_FORMAT_TYPE_FLOAT",
}


def bool_map(value):
    if value:
        return "true"
    else:
        return "false"


swizzle_map = {
    SWIZZLE_X:    "VK_SWIZZLE_X",
    SWIZZLE_Y:    "VK_SWIZZLE_Y",
    SWIZZLE_Z:    "VK_SWIZZLE_Z",
    SWIZZLE_W:    "VK_SWIZZLE_W",
    SWIZZLE_0:    "VK_SWIZZLE_0",
    SWIZZLE_1:    "VK_SWIZZLE_1",
    SWIZZLE_NONE: "VK_SWIZZLE_NONE",
}

def print_channels(format, func):
    if format.nr_channels() <= 1:
        func(format.le_channels, format.le_swizzles)
    else:
        print '#ifdef PIPE_ARCH_BIG_ENDIAN'
        func(format.be_channels, format.be_swizzles)
        print '#else'
        func(format.le_channels, format.le_swizzles)
        print '#endif'

def write_format_table(formats):
    print '/* This file is autogenerated by vk_format_table.py from vk_format_layout.csv. Do not edit directly. */'
    print
    # This will print the copyright message on the top of this file
    print CopyRight.strip()
    print
    print '#include "stdbool.h"'
    print '#include "vk_format.h"'
    print
    
    def do_channel_array(channels, swizzles):
        print "   {"
        for i in range(4):
            channel = channels[i]
            if i < 3:
                sep = ","
            else:
                sep = ""
            if channel.size:
                print "      {%s, %s, %s, %s, %u, %u}%s\t/* %s = %s */" % (type_map[channel.type], bool_map(channel.norm), bool_map(channel.pure), bool_map(channel.scaled), channel.size, channel.shift, sep, "xyzw"[i], channel.name)
            else:
                print "      {0, 0, 0, 0, 0}%s" % (sep,)
        print "   },"

    def do_swizzle_array(channels, swizzles):
        print "   {"
        for i in range(4):
            swizzle = swizzles[i]
            if i < 3:
                sep = ","
            else:
                sep = ""
            try:
                comment = colorspace_channels_map[format.colorspace][i]
            except (KeyError, IndexError):
                comment = 'ignored'
            print "      %s%s\t/* %s */" % (swizzle_map[swizzle], sep, comment)
        print "   },"

    for format in formats:
        print 'static const struct vk_format_description'
        print 'vk_format_%s_description = {' % (format.short_name(),)
        print "   %s," % (format.name,)
        print "   \"%s\"," % (format.name,)
        print "   \"%s\"," % (format.short_name(),)
        print "   {%u, %u, %u},\t/* block */" % (format.block_width, format.block_height, format.block_size())
        print "   %s," % (layout_map(format.layout),)
        print "   %u,\t/* nr_channels */" % (format.nr_channels(),)
        print "   %s,\t/* is_array */" % (bool_map(format.is_array()),)
        print "   %s,\t/* is_bitmask */" % (bool_map(format.is_bitmask()),)
        print "   %s,\t/* is_mixed */" % (bool_map(format.is_mixed()),)
        print_channels(format, do_channel_array)
        print_channels(format, do_swizzle_array)
        print "   %s," % (colorspace_map(format.colorspace),)
        print "};"
        print
        
    print "const struct vk_format_description *"
    print "vk_format_description(VkFormat format)"
    print "{"
    print "   if (format > VK_FORMAT_END_RANGE) {"
    print "      return NULL;"
    print "   }"
    print
    print "   switch (format) {"
    for format in formats:
        print "   case %s:" % format.name
        print "      return &vk_format_%s_description;" % (format.short_name(),)
    print "   default:"
    print "      return NULL;"
    print "   }"
    print "}"
    print


def main():

    formats = []
    for arg in sys.argv[1:]:
        formats.extend(parse(arg))
    write_format_table(formats)


if __name__ == '__main__':
    main()
