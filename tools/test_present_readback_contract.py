"""Source-contract test only; actual Vulkan execution is a separate lab test."""
import argparse
from pathlib import Path
import re

def inspect(source):
    create=source.split('vk::Image CreateWrappedImage(',1)[1].split('void TransitionImageLayout(',1)[0]
    usage=re.search(r'\.usage\s*=\s*(.*?),',create,re.S).group(1)
    download=source.split('void DownloadColorImage(',1)[1].split('vk::ImageView CreateWrappedImageView(',1)[0]
    restore=download.split('const VkImageMemoryBarrier image_write_barrier{',1)[1].split('};',1)[0]
    access=re.search(r'\.dstAccessMask\s*=\s*(.*?),',restore,re.S).group(1)
    assert 'CopyImageToBuffer' in download
    return 'VK_IMAGE_USAGE_TRANSFER_SRC_BIT' in usage, 'VK_ACCESS_MEMORY_READ_BIT' in access

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True)
    p.add_argument('--expect',choices=['baseline','modified'],required=True);a=p.parse_args()
    src,read=inspect(a.source.read_text(encoding='utf-8'))
    assert (src,read)==((False,False) if a.expect=='baseline' else (True,True)),(src,read)
    print(f'SOURCE_CONTRACT_PASS; transfer_src={src}; restore_memory_read={read}; GPU_execution=False')
