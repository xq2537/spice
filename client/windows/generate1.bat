python ..\..\spice-common\spice_codegen.py --generate-demarshallers --client --include messages.h --prefix 1 --ptrsize 8 ..\..\spice-common\spice1.proto ..\generated_demarshallers1.c
python ..\..\spice-common\spice_codegen.py --generate-marshallers -P --include messages.h  --include client_marshallers.h --client --prefix 1 --ptrsize 8 ..\..\spice-common\spice1.proto ..\generated_marshallers1.c
