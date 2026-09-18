#!/usr/bin/env python3
"""Fetch the pinned official SEGGER STM32F7 2 MB dual-bank flash loader."""
from pathlib import Path
import hashlib
from urllib.request import urlopen

URL = 'https://kb.segger.com/images/8/82/ST_STM32F7xxxx_DualBank_2MB.elf'
SHA256 = '478290fb7e4490540c4b54ecb36c391a96d5b4e6a27cc826aaed12a0bb5fd75a'


def main():
    folder = Path(__file__).resolve().parent / 'jlink'
    with urlopen(URL, timeout=30) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise RuntimeError('SEGGER loader hash changed; review the upstream artifact before using it')
    folder.mkdir(exist_ok=True)
    (folder / 'ST_STM32F7xxxx_DualBank_2MB.elf').write_bytes(data)
    (folder / 'JLinkDevices.xml').write_text('''<Database>
 <Device>
  <ChipInfo Vendor="ST" Name="STM32F767ZI" Core="JLINK_CORE_CORTEX_M7" />
  <FlashBankInfo Name="Flash Bank" BaseAddr="0x08000000" AlwaysPresent="1">
   <LoaderInfo Name="DualBank" Loader="ST_STM32F7xxxx_DualBank_2MB.elf" MaxSize="0x00200000" LoaderType="FLASH_ALGO_TYPE_OPEN" />
  </FlashBankInfo>
 </Device>
</Database>
''', encoding='ascii')
    print(folder)


if __name__ == '__main__':
    main()
