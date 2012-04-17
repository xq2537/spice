python ..\..\spice-common\spice_codegen.py --generate-demarshallers  --client --include messages.h ..\..\spice-common\spice.proto ..\generated_demarshallers.c
python ..\..\spice-common\spice_codegen.py --generate-marshallers -P --include messages.h --include client_marshallers.h --client ..\..\spice-common\spice.proto ..\generated_marshallers.c
