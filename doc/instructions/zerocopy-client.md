<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" style="height:64px;margin-right:32px"/>

# I've already used sendmsg with MSG_ZEROCOPY and MSG_ERRQUEUE. I have understood the I need listen to MSG_ERRQUEUE message to free buffer after sending.

However, for receiving by recv MSG_ZEROCOPY with

I have not fully understood why I must listen to MSG_ERRQUEUE ?

MSG_ZEROCOPY allows zero-copy transmission by pinning memory pages and providing asynchronous completion notifications, but it requires the application to track when it is safe to reuse or free the buffer after sending. Listening to MSG_ERRQUEUE (via recvmsg with MSG_ERRQUEUE) is essential because the kernel only notifies the application through this channel when it is safe to modify or free the buffer previously sent with MSG_ZEROCOPY.[^1_3][^1_5]

### Why MSG_ERRQUEUE is Needed

- **Buffer Lifetime**: When using MSG_ZEROCOPY, the application buffer is temporarily shared with the kernel and network stack, instead of being copied. Modifying the buffer immediately after sendmsg returns may corrupt data in transit, since the kernel may not have finished using the pages yet.[^1_5][^1_3]
- **Notification Mechanism**: The kernel sends notifications through MSG_ERRQUEUE to indicate when it has finished with the buffer (i.e., when the memory pages are released). Only after receiving this notification can the application safely reuse or free the buffer.[^1_1][^1_5]
- **No Automatic Freeing**: The kernel never automatically frees or manages user buffers based solely on sendmsg; only the application knows the buffer’s allocation details and must act after safe notification.[^1_2][^1_5]


### recvmsg with MSG_ERRQUEUE

- **recvmsg with MSG_ERRQUEUE** is used to receive those kernel notifications about buffer release. Without handling this, the application risks corruption or premature buffer reuse.[^1_3][^1_5]
- Every successful send is identified in the notification, allowing precise matching between buffers sent and those that are now safe to recycle.[^1_5]

In summary, listening to MSG_ERRQUEUE is obligatory because only through it does the kernel notify when a send buffer previously marked for zero-copy is truly finished and can be safely modified or freed.[^1_3][^1_5]
<span style="display:none">[^1_10][^1_4][^1_6][^1_7][^1_8][^1_9]</span>

<div style="text-align: center">⁂</div>

[^1_1]: https://netdevconf.info/0x18/docs/netdev-0x18-paper14-talk-paper.pdf

[^1_2]: https://stackoverflow.com/questions/21831121/does-sendmsg-free-memory-of-the-buffer-or-msg

[^1_3]: https://docs.kernel.org/networking/msg_zerocopy.html

[^1_4]: https://github.com/boostorg/asio/issues/327

[^1_5]: https://blogs.oracle.com/linux/post/zero-copy-networking-in-uek6

[^1_6]: https://lwn.net/Articles/715279/

[^1_7]: https://netdevconf.info/2.1/papers/debruijn-msgzerocopy-talk.pdf

[^1_8]: https://collaborating.tuhh.de/e-exk4/projects/linux/-/blob/v4.18/Documentation/networking/msg_zerocopy.rst

[^1_9]: https://patchwork.ozlabs.org/patch/808430/

[^1_10]: https://stackoverflow.com/questions/61203647/what-can-cause-returning-so-ee-code-zerocopy-copied-when-sending-udp-messages-wi


---

# On the send/server site, I use std::shared_ptr for being sure that the buffer is not freed before time. Is this possible for receive/client as well?

Using **std::shared_ptr** to manage buffer lifetime on the send side with MSG_ZEROCOPY works well because it helps ensure the buffer is not destroyed until the kernel signals completion, letting the application retain ownership until safe to free.[^2_1]

However, on the **receive side**, the situation is fundamentally different:

- For normal recv/recvmsg, the kernel copies data from its own internal socket buffer into the application's buffer, so there is no shared ownership of the receive buffer between the user and the kernel after the call returns.[^2_2]
- When using modern zero-copy receive APIs (such as TCP zero-copy receive using mmap), buffer reuse is managed by the new API, which maps kernel buffers directly into the user process address space. For such mmap-based receives, the application's buffer must remain valid while the mapped region is in use, but this lifetime is controlled by the kernel's mapping, not a copy operation or shared_ptr semantics.[^2_3]
- Normally, for receive, shared_ptr does not offer special advantages since ordinary recvmsg semantics do not require holding onto the buffer after the call returns; any additional lifetime management is only needed if the kernel's zero-copy receive API requires explicit unmapping or buffer release.[^2_3]

**In summary:** std::shared_ptr is helpful for tracking send buffer lifetime with MSG_ZEROCOPY, but on the receive/client side, it is only meaningful if using an API like mmap or other true zero-copy receive, and then lifetime is managed by kernel mapping logic and not strictly through C++ shared_ptr reference counting. Ordinary socket receive does not require shared ownership semantics for the buffer after the call.[^2_2][^2_3]
<span style="display:none">[^2_10][^2_4][^2_5][^2_6][^2_7][^2_8][^2_9]</span>

<div style="text-align: center">⁂</div>

[^2_1]: https://learn.microsoft.com/en-us/cpp/cpp/how-to-create-and-use-shared-ptr-instances?view=msvc-170

[^2_2]: https://www.kernel.org/doc/html/v6.4/networking/msg_zerocopy.html

[^2_3]: https://blogs.oracle.com/linux/post/zero-copy-networking-in-uek6

[^2_4]: https://stackoverflow.com/questions/5130445/communicating-between-processes-with-shared-memory-results-zero-copy

[^2_5]: https://lwn.net/Articles/879724/

[^2_6]: https://github.com/esnet/iperf/issues/1678

[^2_7]: https://gitlab.amd.e-technik.uni-rostock.de/mesh/apu-linux-kernel/-/blob/b5ee153276dbd4e144c02f50cead17c03c75a36a/Documentation/networking/msg_zerocopy.rst

[^2_8]: https://www.reddit.com/r/cpp_questions/comments/v37d0w/understanding_shared_ptrreset/

[^2_9]: https://twdev.blog/2024/09/sharedptr/

[^2_10]: https://www.reddit.com/r/cpp_questions/comments/q647ms/how_to_copy_and_reference_a_shared_ptr/


---

# I use recv with MSG_ZEROCOPY and MSG_ERRQUEUE, as said before. Hence std::shared_ptr is not needed in this case?

Using **recv** with MSG_ZEROCOPY and MSG_ERRQUEUE does not require std::shared_ptr or similar smart-pointer lifetime management for the receive buffer. This is because, even when requesting zero-copy semantics for sending, the receive operation in standard Linux sockets works differently: the kernel copies data into the application's buffer and relinquishes control once recvmsg returns. The user is then free to reuse or free the buffer as usual, with no shared ownership or deferred completion notification needed for receive, only for send.[^3_1][^3_2]

**Summary of requirements:**

- **Send (MSG_ZEROCOPY):** Shared ownership and deferred buffer management are needed until notification via MSG_ERRQUEUE.
- **Receive:** No deferred notification; buffer can be reused or freed as soon as recvmsg returns.[^3_2]

Therefore, **std::shared_ptr is not needed for receive buffer lifetime management with recv and MSG_ZEROCOPY**—only conventional buffer management is required.[^3_1][^3_2]
<span style="display:none">[^3_10][^3_11][^3_12][^3_13][^3_14][^3_15][^3_16][^3_17][^3_18][^3_19][^3_20][^3_3][^3_4][^3_5][^3_6][^3_7][^3_8][^3_9]</span>

<div style="text-align: center">⁂</div>

[^3_1]: https://docs.kernel.org/networking/msg_zerocopy.html

[^3_2]: https://stackoverflow.com/questions/9393548/recv-packets-with-pointer-members

[^3_3]: https://www.reddit.com/r/cpp_questions/comments/w9036e/zmq_sending_a_shared_ptr/

[^3_4]: https://lwn.net/Articles/726917/

[^3_5]: https://lwn.net/Articles/879724/

[^3_6]: https://www.kernel.org/doc/html/v4.15/networking/msg_zerocopy.html

[^3_7]: https://stackoverflow.com/questions/62578197/zerocopy-tcp-message-sending-from-kernel-module-error-efault

[^3_8]: https://netdevconf.info/0x18/docs/netdev-0x18-paper14-talk-paper.pdf

[^3_9]: https://stackoverflow.com/questions/17420056/zero-copy-receive-function

[^3_10]: https://patchwork.ozlabs.org/patch/808430/

[^3_11]: https://github.com/esnet/iperf/issues/1678

[^3_12]: https://gitlab.cs.fau.de/sa83dova/linux/-/blob/a8750ddca918032d6349adbf9a4b6555e7db20da/Documentation/networking/msg_zerocopy.rst

[^3_13]: https://gitlab.amd.e-technik.uni-rostock.de/mesh/apu-linux-kernel/-/blob/b5ee153276dbd4e144c02f50cead17c03c75a36a/Documentation/networking/msg_zerocopy.rst

[^3_14]: https://gitlab.cs.fau.de/sa83dova/linux/-/blob/v5.4-rc4/Documentation/networking/msg_zerocopy.rst

[^3_15]: https://lwn.net/Articles/752188/

[^3_16]: https://stackoverflow.com/questions/48861927/how-do-i-get-the-size-of-the-msg-control-buffer-for-recvmsg

[^3_17]: https://en.cppreference.com/w/cpp/memory/shared_ptr.html

[^3_18]: https://netdevconf.info/2.1/papers/debruijn-msgzerocopy-talk.pdf

[^3_19]: https://www.kernel.org/doc/html/v5.16/networking/kapi.html

[^3_20]: https://stackoverflow.com/questions/58627200/zero-copy-user-space-tcp-send-of-dma-mmap-coherent-mapped-memory


---

# There is an implicit context here:

* TCP zero-copy receive using mmap seems to be (much?) better than
* recv with MSG_ZEROCOPY and MSG_ERRQUEUE

Is that right? If so, please provide an TCP zero-copy receive using mmap example in C++!

Yes, **TCP zero-copy receive using mmap** is generally considered superior to using recv with MSG_ZEROCOPY and MSG_ERRQUEUE for receive-side zero-copy performance, because mmap allows the kernel to map packet data directly into user-space without a traditional user/kernel buffer copy. However, this technique is advanced and only works when the incoming data is both page-aligned and a multiple of the page size—otherwise, the fallback is to use regular recvmsg.[^4_1][^4_2]

### Comparison: mmap-based vs MSG_ZEROCOPY recv

| Feature | mmap-based TCP Zero-Copy Receive | recv + MSG_ZEROCOPY + MSG_ERRQUEUE |
| :-- | :-- | :-- |
| Zero-copy on receive | Yes (when conditions are met)[^4_1][^4_2] | No (only applies to send side)[^4_3] |
| Best for | High-performance, page-sized aligned transfers[^4_2] | Standard applications, easier integration |
| Buffer can be accessed in-place | Yes | No |
| Kernel/user copy avoided | Yes (when aligned)[^4_1][^4_2] | No |

### Example: TCP Zero-Copy Receive Using mmap in C++

```cpp
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <linux/tcp.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>

#define PAGE_SIZE 4096

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    // Setup TCP server socket (bind/listen/accept...)
    // Assume client connects and server_fd is ready after accept

    // Reserve space for mapping
    void* mapped_mem = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    // Create and use a tcp_zerocopy_receive struct
    struct tcp_zerocopy_receive zc = {};
    zc.address = (uint64_t)mapped_mem;
    zc.length = PAGE_SIZE;

    // Ask kernel to map received data into this region.
    socklen_t optlen = sizeof(zc);
    int ret = getsockopt(server_fd, IPPROTO_TCP, TCP_ZEROCOPY_RECEIVE, &zc, &optlen);
    if (ret == 0 && zc.length > 0) {
        // Data is now mapped into mapped_mem, process it directly
        printf("Received %u bytes zero-copy mapped data!\n", zc.length);

        // Use data: char* data = (char*)mapped_mem;

        // When done, free the mapping if done with this buffer
        munmap(mapped_mem, PAGE_SIZE);
    } else {
        perror("TCP_ZEROCOPY_RECEIVE failed or no data available");
        // Fallback to recv/recvmsg()
    }
    close(server_fd);
    return 0;
}
```

- Code assumes kernel and libc with support for TCP_ZEROCOPY_RECEIVE and the required struct definition.[^4_2][^4_1]
- Always provide a page-aligned, page-sized buffer. If getsockopt fails (EINVAL), fall back on ordinary receive.[^4_2]

**Note:** This is a low-level technique and current support/portability may vary across kernel versions; always consult your system's kernel and library documentation for production use.[^4_1][^4_2]
<span style="display:none">[^4_10][^4_11][^4_12][^4_13][^4_14][^4_15][^4_16][^4_17][^4_4][^4_5][^4_6][^4_7][^4_8][^4_9]</span>

<div style="text-align: center">⁂</div>

[^4_1]: https://lwn.net/Articles/754681/

[^4_2]: https://lwn.net/Articles/752188/

[^4_3]: https://docs.kernel.org/networking/msg_zerocopy.html

[^4_4]: https://blogs.oracle.com/linux/post/zero-copy-networking-in-uek6

[^4_5]: https://people.kernel.org/kuba/musings-on-zero-copy-tcp

[^4_6]: https://stackoverflow.com/questions/58627200/zero-copy-user-space-tcp-send-of-dma-mmap-coherent-mapped-memory

[^4_7]: https://groups.google.com/g/mechanical-sympathy/c/6v1nMuuTd_g

[^4_8]: https://patchwork.ozlabs.org/patch/904003/

[^4_9]: https://www.linuxjournal.com/article/6345

[^4_10]: https://catonmat.net/zero-copy-loopback-stack

[^4_11]: https://www.kernel.org/doc/html/v6.1/networking/msg_zerocopy.html

[^4_12]: https://yarchive.net/comp/linux/zero-copy.html

[^4_13]: https://lwn.net/Articles/726917/

[^4_14]: https://legacy.netdevconf.info/0x14/pub/slides/62/Implementing TCP RX zero copy.pdf

[^4_15]: https://www.sobyte.net/post/2022-11/zero-copy/

[^4_16]: https://netdevconf.info/0x15/papers/1/maio_netdev0x15.pdf

[^4_17]: https://codemia.io/blog/path/Zero-Copy-IO-From-sendfile-to-iouring--Evolution-and-Impact-on-Latency-in-Distributed-Logs


---

# So for mmap-based TCP Zero-Copy Receive: What a the conditions? And what is the alignment requirement here?

For **mmap-based TCP zero-copy receive**, strict conditions must be met for true zero-copy to occur:

### Conditions and Alignment Requirements

- **Page Alignment**: The user-supplied buffer's address (passed to the kernel for mapping) must be page-aligned. This requirement is automatically satisfied if the address comes from mmap with regular settings.[^5_2][^5_3]
- **Page Size Multiples**: The requested buffer length must be a multiple of the system page size (typically 4096 bytes).[^5_3][^5_2]
- **Packet Alignment**: The incoming TCP payload must be page-aligned and exactly fill one or more pages. This means the kernel can only zero-copy map the data if it arrives in units that match system page boundaries—otherwise, the call will fail, and the application should fall back to recv().[^5_3]
- **MTU and Protocol Headers**: The network’s MTU and header overhead must be configured so that after headers, data fills a whole page. If TCP/IP headers and options result in less than the expected payload per page, true zero-copy may not be possible for every packet.[^5_5]
- **No Urgent Data or Out-of-Band Data**: If there is urgent data queued, or TCP options/segment structure interfere, the mapping will not succeed and must fall back to normal copy.[^5_3]
- **Single-threaded or High-Throughput Applications**: The mechanism is generally effective in applications with high, steady receive bandwidth, large payloads, and a single receiving socket.[^5_4]


### Example Alignment Calculation

If the system page size is 4096, then:

- Buffer address for mmap must be page-aligned (e.g., 0x100000, 0x101000, ...).
- Requested size should be a multiple of 4096 (4096, 8192, etc.).
- The TCP stack and application should coordinate to send payloads that match these boundaries for maximal effectiveness.


### Practical Notes

- If alignment is not exact or if only part of a page is available, the zero-copy receive may not work; kernel returns EINVAL and the user must fall back to normal recv().[^5_2][^5_3]
- Proper tuning of MTU, TCP_MAXSEG, and message sizes is necessary for optimal use.[^5_5]

**In summary:** mmap-based TCP zero-copy receive requires both the user's buffer and the incoming data to be strictly page-aligned and sized, with buffer sizes in multiples of the system page size for the mapping to succeed without copy.[^5_2][^5_5][^5_3]
<span style="display:none">[^5_1][^5_10][^5_6][^5_7][^5_8][^5_9]</span>

<div style="text-align: center">⁂</div>

[^5_1]: https://blogs.oracle.com/linux/post/zero-copy-networking-in-uek6

[^5_2]: https://lwn.net/Articles/754681/

[^5_3]: https://lwn.net/Articles/752188/

[^5_4]: https://patchwork.ozlabs.org/patch/904003/

[^5_5]: https://legacy.netdevconf.info/0x14/pub/slides/62/Implementing TCP RX zero copy.pdf

[^5_6]: https://docs.kernel.org/networking/iou-zcrx.html

[^5_7]: https://goperf.dev/01-common-patterns/zero-copy/

[^5_8]: https://docs.kernel.org/networking/packet_mmap.html

[^5_9]: https://stackoverflow.com/questions/77425204/how-do-zero-copy-networking-libraries-avoid-copies-when-they-read-write-packets

[^5_10]: https://static.aminer.org/pdf/PDF/000/253/158/design_and_implementation_of_zero_copy_data_path_for_efficient.pdf

