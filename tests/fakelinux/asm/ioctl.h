#ifndef FAKE_ASM_IOCTL_H
#define FAKE_ASM_IOCTL_H
#define _IOC(d, t, n, s) ((unsigned int)(((d) << 30) | ((s) << 16) | ((t) << 8) | (n)))
#define _IOR(t, n, s)  _IOC(2u, (t), (n), sizeof(s))
#define _IOW(t, n, s)  _IOC(1u, (t), (n), sizeof(s))
#define _IOWR(t, n, s) _IOC(3u, (t), (n), sizeof(s))
#define _IO(t, n)      _IOC(0u, (t), (n), 0)
#endif
