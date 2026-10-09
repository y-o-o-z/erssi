# this file is part of irssi

# The protocol code without irssi: crypto, JSON, wire format, keyring file,
# handshake. A static library so tests/e2e and the fuzz targets
# (src/fe-fuzz/e2e) use exactly what the module runs.
e2e_proto_sources = files(
  'e2e-crypto.c',
  'e2e-wire.c',
  'e2e-json.c',
  'e2e-keyring.c',
  'e2e-proto.c',
)

libe2e_proto_a = static_library('e2e_proto',
  e2e_proto_sources,
  include_directories : rootinc,
  implicit_include_directories : false,
  dependencies : dep,
  pic : true,
)

# the module, loaded like otr (autoload_modules has "e2e")
shared_library('e2e_core',
  files(
    'e2e-commands.c',
    'e2e-core.c',
    'e2e-formats.c',
  ),
  link_with : libe2e_proto_a,
  include_directories : rootinc,
  implicit_include_directories : false,
  name_suffix : module_suffix,
  install : true,
  install_dir : moduledir,
  dependencies : dep,
  override_options : ['b_lundef=false'],
)

# noinst_headers = files(
#   'e2e-core.h',
#   'e2e-crypto.h',
#   'e2e-formats.h',
#   'e2e-json.h',
#   'e2e-keyring.h',
#   'e2e-proto.h',
#   'e2e-wire.h',
#   'module.h',
# )
