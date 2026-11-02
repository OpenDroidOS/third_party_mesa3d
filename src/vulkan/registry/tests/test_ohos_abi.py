#!/usr/bin/env python3
# Copyright 2026 OpenDroidOS
# SPDX-License-Identifier: Apache-2.0

"""Check the OpenHarmony loader/driver wire ABI and registry consumers."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[4]
REGISTRY = ROOT / 'src/vulkan/registry/vk.xml'
# Values in the native OpenHarmony Vulkan headers used by its swapchain layer.
# They intentionally differ from offsets derived from extension number 687.
STRUCTURE_VALUES = {
    'VK_STRUCTURE_TYPE_NATIVE_BUFFER_OHOS': 1000453001,
    'VK_STRUCTURE_TYPE_SWAPCHAIN_IMAGE_CREATE_INFO_OHOS': 1000453002,
    'VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENTATION_PROPERTIES_OHOS': 1000453003,
}
COMMANDS = {
    'vkGetSwapchainGrallocUsageOHOS',
    'vkAcquireImageOHOS',
    'vkQueueSignalReleaseImageOHOS',
}


def abi_probe(cpp):
    assertion = 'static_assert' if cpp else '_Static_assert'
    text = '#include <stddef.h>\n#include <stdint.h>\n#include <vulkan/vulkan.h>\n'
    for name, value in STRUCTURE_VALUES.items():
        text += f'{assertion}({name} == {value}, "native structure value");\n'
    text += f'''
{assertion}(VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS == 1000685000, "surface ABI");
{assertion}(VK_SWAPCHAIN_IMAGE_USAGE_SHARED_BIT_OHOS == 1, "shared-image bit");
{assertion}(VK_OHOS_NATIVE_BUFFER_SPEC_VERSION == 1, "native-buffer contract version");
{assertion}(sizeof(VkNativeBufferOHOS) == (sizeof(void *) == 8 ? 24 : 12), "buffer layout");
{assertion}(offsetof(VkNativeBufferOHOS, sType) == 0, "buffer type offset");
{assertion}(offsetof(VkNativeBufferOHOS, pNext) == (sizeof(void *) == 8 ? 8 : 4), "next offset");
{assertion}(offsetof(VkNativeBufferOHOS, handle) == (sizeof(void *) == 8 ? 16 : 8), "handle offset");
{assertion}(sizeof(VkSwapchainImageCreateInfoOHOS) == (sizeof(void *) == 8 ? 24 : 12), "usage layout");
{assertion}(sizeof(VkPhysicalDevicePresentationPropertiesOHOS) == (sizeof(void *) == 8 ? 24 : 12), "properties layout");
'''
    prototypes = {
        'PFN_vkGetSwapchainGrallocUsageOHOS':
            'VkResult (VKAPI_PTR *)(VkDevice, VkFormat, VkImageUsageFlags, uint64_t *)',
        'PFN_vkAcquireImageOHOS':
            'VkResult (VKAPI_PTR *)(VkDevice, VkImage, int32_t, VkSemaphore, VkFence)',
        'PFN_vkQueueSignalReleaseImageOHOS':
            'VkResult (VKAPI_PTR *)(VkQueue, uint32_t, const VkSemaphore *, VkImage, int32_t *)',
    }
    if cpp:
        text += '#include <type_traits>\n'
    for name, prototype in prototypes.items():
        if cpp:
            check = f'(std::is_same<{name}, {prototype}>::value)'
        else:
            check = f'__builtin_types_compatible_p({name}, {prototype})'
        text += f'{assertion}({check}, "{name} signature");\n'
    return text


class OhosAbiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.registry = ET.parse(REGISTRY).getroot()

    def test_registry_uses_deployed_structure_values(self):
        extension = self.registry.find('./extensions/extension[@name="VK_OHOS_native_buffer"]')
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get('platform'), 'ohos')
        self.assertEqual(extension.get('type'), 'device')
        for name, value in STRUCTURE_VALUES.items():
            nodes = extension.findall(f'./require/enum[@name="{name}"]')
            self.assertEqual(len(nodes), 1)
            self.assertEqual(int(nodes[0].get('value')), value)
            self.assertNotIn('offset', nodes[0].attrib)
        self.assertEqual({node.get('name') for node in extension.findall('./require/command')}, COMMANDS)

    def test_bundled_core_values_match_registry(self):
        core = (ROOT / 'include/vulkan/vulkan_core.h').read_text()
        for name, value in STRUCTURE_VALUES.items():
            declarations = re.findall(r'\b' + name + r'\s*=\s*([0-9]+)', core)
            self.assertEqual(declarations, [str(value)])

    def compile_probe(self, cpp):
        compiler = shutil.which('c++' if cpp else 'cc')
        self.assertIsNotNone(compiler, 'A host C/C++ compiler is required')
        headers = [ROOT / 'include']
        if os.environ.get('OHOS_VULKAN_HEADERS'):
            headers.append(Path(os.environ['OHOS_VULKAN_HEADERS']))
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / ('abi.cpp' if cpp else 'abi.c')
            source.write_text(abi_probe(cpp))
            for include in headers:
                with self.subTest(headers=str(include)):
                    result = subprocess.run([compiler, '-std=c++17' if cpp else '-std=c11',
                                             '-Wall', '-Wextra', '-Werror', '-DVK_USE_PLATFORM_OHOS',
                                             '-I' + str(include), '-c', str(source),
                                             '-o', str(Path(temp) / 'abi.o')],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_c_wire_abi(self):
        self.compile_probe(False)

    def test_cpp_wire_abi(self):
        self.compile_probe(True)

    def test_real_dispatch_generator_sees_native_commands(self):
        generator = ROOT / 'src/vulkan/util/vk_dispatch_table_gen.py'
        with tempfile.TemporaryDirectory() as temp:
            header = Path(temp) / 'dispatch.h'
            source = Path(temp) / 'dispatch.c'
            result = subprocess.run([sys.executable, str(generator), '--xml', str(REGISTRY),
                                     '--beta', 'false', '--out-h', str(header), '--out-c', str(source)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            text = header.read_text()
            for command in COMMANDS:
                self.assertIn('PFN_' + command, text)
            self.assertIn('VK_USE_PLATFORM_OHOS', text)


if __name__ == '__main__':
    unittest.main()
