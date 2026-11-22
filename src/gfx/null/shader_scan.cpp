// Investigation aid, active only with D3_SHADER_DUMP=<folder>.
//
// The driver is handed a shader's GPU code and control block, but not the rest of
// the compiled-shader file the game loaded them from, which also carries the
// reflection data (names and locations of inputs, uniforms and samplers). This
// finds those files in the game's memory and writes each to <folder>/blob_NNNN.glslc.
#include "nvn_null.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>

void d3_shader_scan(const char * folder)
{
    const uint32_t kMagic = 0x98761234;
    mach_vm_address_t address = 0;
    int found = 0;
    for (;;)
    {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object;
        if (mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info),
                           &count, &object) != KERN_SUCCESS)
        {
            break;
        }
        if ((info.protection & VM_PROT_READ) && (info.protection & VM_PROT_WRITE))
        {
            const uint8_t * p = reinterpret_cast<const uint8_t *>(address);
            for (mach_vm_size_t off = 0; off + 0x120 <= size; off += 8)
            {
                uint32_t magic, total, dataOffset, sections;
                memcpy(&magic, p + off, 4);
                if (magic != kMagic)
                {
                    continue;
                }
                memcpy(&total, p + off + 0x44, 4);
                memcpy(&dataOffset, p + off + 0x48, 4);
                memcpy(&sections, p + off + 0x4c, 4);
                if (sections < 1 || sections > 8 || dataOffset != 0x90 + 0x90 * sections || total <= dataOffset || total > 0x400000 ||
                    off + total > size)
                {
                    continue;
                }
                char path[1024];
                snprintf(path, sizeof path, "%s/blob_%04d.glslc", folder, found++);
                if (FILE * f = fopen(path, "wb"))
                {
                    fwrite(p + off, 1, total, f);
                    fclose(f);
                }
                off += (total & ~7u);
            }
        }
        address += size;
    }
    fprintf(stderr, "shader scan: %d compiled-shader files written to %s\n", found, folder);
}
#else
void d3_shader_scan(const char *) {}
#endif
