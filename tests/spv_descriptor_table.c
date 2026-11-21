// Donor nxnative 1f01e28c validation driver.
#include "spv_descriptor_table.h"
#include <assert.h>
#include <stdio.h>

int main(int argc,char **argv) {
    assert(argc>=6 && (argc-3)%3==0);
    size_t edit_count=(argc-3)/3;
    descriptor_table_edit *edits=calloc(edit_count,sizeof *edits);assert(edits);
    for (size_t i=0;i<edit_count;i++) edits[i]=(descriptor_table_edit){
        (uint32_t)strtoul(argv[3+3*i],NULL,10),(uint32_t)strtoul(argv[4+3*i],NULL,10),(uint32_t)strtoul(argv[5+3*i],NULL,10)};
    FILE *file=fopen(argv[1],"rb");assert(file);
    assert(!fseek(file,0,SEEK_END));long bytes=ftell(file);rewind(file);
    assert(bytes>=20 && bytes%4==0);
    uint32_t *input=malloc(bytes);assert(input);
    assert(fread(input,1,bytes,file)==(size_t)bytes);fclose(file);
    size_t count;const char *reason;
    assert(!spv_descriptor_table(input,4,edits,edit_count,&count,&reason) && reason);
    descriptor_table_edit missing={0xffff,0xfffe,301};
    assert(!spv_descriptor_table(input,bytes/4,&missing,1,&count,&reason) && reason);
    uint32_t *output=spv_descriptor_table(input,bytes/4,edits,edit_count,&count,&reason);
    if (!output) { fprintf(stderr,"%s\n",reason);free(input);free(edits);return 1; }
    file=fopen(argv[2],"wb");assert(file);
    assert(fwrite(output,4,count,file)==count);fclose(file);
    free(input);free(output);free(edits);return 0;
}
