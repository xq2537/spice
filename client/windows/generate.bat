python ..\..\spice_codegen.py -d -c  -i common.h -i messages.h ..\..\spice.proto ..\generated_demarshallers.cpp
python ..\..\spice_codegen.py --generate-marshallers --include messages.h --client ..\..\spice.proto ..\generated_marshallers.cpp
python ..\..\spice_codegen.py --generate-marshallers --client -H ..\..\spice.proto ..\generated_marshallers.h
