python ..\..\spice_codegen.py -d -c  -i common.h -i messages.h ..\..\spice.proto ..\generated_demarshallers.cpp
python ..\..\spice_codegen.py --generate-marshallers -P --include "common.h" --include messages.h --include marshallers.h --client ..\..\spice.proto ..\generated_marshallers.cpp
