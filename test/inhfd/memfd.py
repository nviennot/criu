import os
import ctypes
import subprocess
libc = ctypes.CDLL(None)


# The SYS_memfd_create value depends on the architecture.
# We don't have a good way to get the system call number.
# Here, we invoke gcc. Oh well.
def __get_SYS_memfd_create():
    prog = """
    #include <sys/syscall.h>
    #include <stdio.h>
    int main() {
      printf("%d", SYS_memfd_create);
      return 0;
    }
    """
    cmd = "echo '{}' | gcc -x c - -o memfd_out && ./memfd_out && rm -f ./memfd_out".format(prog)
    out = subprocess.check_output(cmd, shell=True)
    return int(out)


SYS_memfd_create = __get_SYS_memfd_create()


def memfd_create(name, flags):
    return libc.syscall(SYS_memfd_create, name.encode('utf8'), flags)


def create_fds():
    def create_memfd_pair(name):
        fd = memfd_create(name, 0)
        fw = open('/proc/self/fd/{}'.format(fd), 'wb')
        fr = open('/proc/self/fd/{}'.format(fd), 'rb')
        os.close(fd)
        return (fw, fr)

    return [create_memfd_pair("name{}".format(i)) for i in range(10)]


def filename(f):
    name = os.readlink('/proc/self/fd/{}'.format(f.fileno()))
    name = name.replace(' (deleted)', '')
    return name


def dump_opts(sockf):
    return []
