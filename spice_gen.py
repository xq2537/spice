#!/usr/bin/env python

import os
import sys
from optparse import OptionParser
import traceback
from python_modules import spice_parser
from python_modules import ptypes
from python_modules import codegen
from python_modules import demarshal

def write_channel_enums(writer, channel, client):
    messages = filter(lambda m : m.channel == channel, \
                          channel.client_messages if client else channel.server_messages)
    if len(messages) == 0:
        return
    writer.begin_block("enum")
    i = 0;
    if client:
        prefix = [ "MSGC" ]
    else:
        prefix = [ "MSG" ]
    if channel.member_name:
        prefix.append(channel.member_name.upper())
    prefix.append(None) # To be replaced with name
    for m in messages:
        prefix[-1] = m.name.upper()
        enum = codegen.prefix_underscore_upper(*prefix)
        if m.value == i:
            writer.writeln("%s," % enum)
            i = i + 1
        else:
            writer.writeln("%s = %s," % (enum, m.value))
            i = m.value + 1
    if channel.member_name:
        prefix[-1] = prefix[-2]
        prefix[-2] = "END"
        writer.newline()
        writer.writeln("%s" % (codegen.prefix_underscore_upper(*prefix)))
    writer.end_block(semicolon=True)
    writer.newline()

def write_enums(writer):
    writer.writeln("#ifndef _H_SPICE_ENUMS")
    writer.writeln("#define _H_SPICE_ENUMS")
    writer.newline()
    writer.comment("Generated from %s, don't edit" % writer.options["source"]).newline()
    writer.newline()

    # Define enums
    for t in ptypes.get_named_types():
        if isinstance(t, ptypes.EnumBaseType):
            t.c_define(writer)

    i = 0;
    writer.begin_block("enum")
    for c in proto.channels:
        enum = codegen.prefix_underscore_upper("CHANNEL", c.name.upper())
        if c.value == i:
            writer.writeln("%s," % enum)
            i = i + 1
        else:
            writer.writeln("%s = %s," % (enum, c.value))
            i = c.value + 1
    writer.newline()
    writer.writeln("SPICE_END_CHANNEL")
    writer.end_block(semicolon=True)
    writer.newline()

    for c in ptypes.get_named_types():
        if not isinstance(c, ptypes.ChannelType):
            continue
        write_channel_enums(writer, c, False)
        write_channel_enums(writer, c, True)

    writer.writeln("#endif /* _H_SPICE_ENUMS */")

parser = OptionParser(usage="usage: %prog [options] <protocol_file> <destination file>")
parser.add_option("-e", "--generate-enums",
                  action="store_true", dest="generate_enums", default=False,
                  help="Generate enums")
parser.add_option("-d", "--generate-demarshallers",
                  action="store_true", dest="generate_demarshallers", default=False,
                  help="Generate demarshallers")
parser.add_option("-a", "--assert-on-error",
                  action="store_true", dest="assert_on_error", default=False,
                  help="Assert on error")
parser.add_option("-p", "--print-error",
                  action="store_true", dest="print_error", default=False,
                  help="Print errors")
parser.add_option("-s", "--server",
                  action="store_true", dest="server", default=False,
                  help="Print errors")
parser.add_option("-c", "--client",
                  action="store_true", dest="client", default=False,
                  help="Print errors")
parser.add_option("-k", "--keep-identical-file",
                  action="store_true", dest="keep_identical_file", default=False,
                  help="Print errors")
parser.add_option("-i", "--include",
                  dest="include", default=None, metavar="FILE",
                  help="Include FILE in generated code")

(options, args) = parser.parse_args()

if len(args) == 0:
    parser.error("No protocol file specified")

if len(args) == 1:
    parser.error("No destination file specified")

proto_file = args[0]
dest_file = args[1]
proto = spice_parser.parse(proto_file)

if proto == None:
    exit(1)

codegen.set_prefix(proto.name)
writer = codegen.CodeWriter()
writer.set_option("source", os.path.basename(proto_file))

if options.assert_on_error:
    writer.set_option("assert_on_error")

if options.print_error:
    writer.set_option("print_error")

if options.include:
    writer.writeln('#include "%s"' % options.include)

if options.generate_enums:
    write_enums(writer)

if options.generate_demarshallers:
    if not options.server and not options.client:
        print >> sys.stderr, "Must specify client and/or server"
        sys.exit(1)
    demarshal.write_includes(writer)

    if options.server:
        demarshal.write_protocol_parser(writer, proto, False)
    if options.client:
        demarshal.write_protocol_parser(writer, proto, True)

content = writer.getvalue()
if options.keep_identical_file:
    try:
        f = open(dest_file, 'rb')
        old_content = f.read()
        f.close()

        if content == old_content:
            print "No changes to %s" % dest_file
            sys.exit(0)

    except IOError:
        pass

f = open(dest_file, 'wb')
f.write(content)
f.close()

print "Wrote %s" % dest_file
sys.exit(0)
