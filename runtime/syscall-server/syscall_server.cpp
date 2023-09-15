#include "syscall_server.hpp"
#include <cstring>

using namespace bpftime;

const shm_open_type bpftime::global_shm_open_type = shm_open_type::SHM_SERVER;

// global context for bpf syscall server
static syscall_context context;

#define PERF_UPROBE_REF_CTR_OFFSET_BITS 32
#define PERF_UPROBE_REF_CTR_OFFSET_SHIFT 32

/*
 * this function is expected to parse integer in the range of [0, 2^31-1] from
 * given file using scanf format string fmt. If actual parsed value is
 * negative, the result might be indistinguishable from error
 */
static int parse_uint_from_file(const char *file, const char *fmt)
{
	int err, ret;
	FILE *f;

	f = fopen(file, "re");
	if (!f) {
		err = -errno;
		std::cerr << "failed to open '" << file << "': " << err
			  << std::endl;
		return err;
	}
	err = fscanf(f, fmt, &ret);
	if (err != 1) {
		err = err == EOF ? -EIO : -errno;
		std::cerr << "failed to parse '" << file << "': " << err
			  << std::endl;
		fclose(f);
		return err;
	}
	fclose(f);
	return ret;
}

static int determine_uprobe_perf_type(void)
{
	const char *file = "/sys/bus/event_source/devices/uprobe/type";

	return parse_uint_from_file(file, "%d\n");
}

static int determine_uprobe_retprobe_bit(void)
{
	const char *file =
		"/sys/bus/event_source/devices/uprobe/format/retprobe";

	return parse_uint_from_file(file, "config:%d\n");
}

extern "C" int epoll_wait(int epfd, epoll_event *evt, int maxevents,
			  int timeout)
{
	std::cout << "epoll_wait " << epfd << std::endl;
	return context.handle_epoll_wait(epfd, evt, maxevents, timeout);
}

extern "C" int epoll_ctl(int epfd, int op, int fd, epoll_event *evt)
{
	std::cout << "epoll_ctl " << epfd << " " << op << " " << fd << " "
		  << evt << std::endl;
	return context.handle_epoll_ctl(epfd, op, fd, evt);
}

extern "C" int epoll_create1(int flags)
{
	std::cout << "epoll_create1 " << flags << std::endl;
	return context.handle_epoll_create1(flags);
}

extern "C" int ioctl(int fd, unsigned long req, int data)
{
	std::cout << "ioctl " << fd << " " << req << " " << data << std::endl;
	return context.handle_ioctl(fd, req, data);
}

extern "C" void *mmap64(void *addr, size_t length, int prot, int flags, int fd,
			off64_t offset)
{
	std::cout << "Mmap64 " << addr << std::endl;
	return context.handle_mmap64(addr, length, prot, flags, fd, offset);
}

extern "C" int close(int fd)
{
	std::cout << "Closing " << fd << std::endl;
	return context.handle_close(fd);
}

extern "C" long syscall(long sysno, ...)
{
	// glibc directly reads the arguments without considering
	// the underlying argument number. So did us
	va_list args;
	va_start(args, sysno);
	long arg1 = va_arg(args, long);
	long arg2 = va_arg(args, long);
	long arg3 = va_arg(args, long);
	long arg4 = va_arg(args, long);
	long arg5 = va_arg(args, long);
	long arg6 = va_arg(args, long);
	va_end(args);
	if (sysno == __NR_bpf) {
		std::cout << "SYS_BPF"
			  << " " << arg1 << " " << arg2 << " " << arg3 << " "
			  << arg4 << " " << arg5 << " " << arg6 << std::endl;
		int cmd = (int)arg1;
		auto attr = (union bpf_attr *)(uintptr_t)arg2;
		auto size = (size_t)arg3;
		return context.handle_sysbpf(cmd, attr, size);
	} else if (sysno == __NR_perf_event_open) {
		std::cout << "SYS_PERF_EVENT_OPEN"
			  << " " << arg1 << " " << arg2 << " " << arg3 << " "
			  << arg4 << " " << arg5 << " " << arg6 << std::endl;
		return context.handle_perfevent(
			(perf_event_attr *)(uintptr_t)arg1, (pid_t)arg2,
			(int)arg3, (int)arg4, (unsigned long)arg5);
	} else if (sysno == __NR_ioctl) {
		std::cout << "SYS_IOCTL"
			  << " " << arg1 << " " << arg2 << " " << arg3 << " "
			  << arg4 << " " << arg5 << " " << arg6 << std::endl;
	}
	return context.orig_syscall_fn(sysno, arg1, arg2, arg3, arg4, arg5,
				       arg6);
}

int syscall_context::handle_close(int fd)
{
	bpftime_close(fd);
	return orig_close_fn(fd);
}

long syscall_context::handle_sysbpf(int cmd, union bpf_attr *attr, size_t size)
{
	errno = 0;
	char *errmsg;
	switch (cmd) {
	case BPF_MAP_CREATE: {
		std::cout << "Creating map" << std::endl;
		int id = bpftime_maps_create(
			attr->map_name, bpftime::bpf_map_attr{
						(int)attr->map_type,
						attr->key_size,
						attr->value_size,
						attr->max_entries,
						attr->map_flags,
						attr->map_ifindex,
						attr->btf_vmlinux_value_type_id,
						attr->btf_id,
						attr->btf_key_type_id,
						attr->btf_value_type_id,
						attr->map_extra,
					});
		std::cout << "Created map " << id << std::endl;
		return id;
	}
	case BPF_MAP_LOOKUP_ELEM: {
		std::cout << "Looking up map" << std::endl;
		// Note that bpftime_map_lookup_elem is adapted as a bpf helper,
		// meaning that it will *return* the address of the matched
		// value. But here the syscall has a different interface. Here
		// we should write the bytes of the matched value to the pointer
		// that user gave us. So here needs a memcpy to achive such
		// thing.
		auto value_ptr = bpftime_map_lookup_elem(
			attr->map_fd, (const void *)(uintptr_t)attr->key);
		memcpy((void *)(uintptr_t)attr->value, value_ptr,
		       bpftime_map_value_size(attr->map_fd));
		return 0;
	}
	case BPF_MAP_UPDATE_ELEM: {
		std::cout << "Updating map" << std::endl;
		return bpftime_map_update_elem(
			attr->map_fd, (const void *)(uintptr_t)attr->key,
			(const void *)(uintptr_t)attr->value,
			(uint64_t)attr->flags);
	}
	case BPF_MAP_DELETE_ELEM: {
		std::cout << "Deleting map" << std::endl;
		return bpftime_map_delete_elem(
			attr->map_fd, (const void *)(uintptr_t)attr->key);
	}
	case BPF_MAP_GET_NEXT_KEY: {
		std::cout << "Getting next key" << std::endl;
		return (long)(uintptr_t)bpftime_map_get_next_key(
			attr->map_fd, (const void *)(uintptr_t)attr->key,
			(void *)(uintptr_t)attr->next_key);
	}
	case BPF_PROG_LOAD:
		// Load a program?
		{
			std::cout << "Loading program `" << attr->prog_name
				  << "` license `"
				  << (const char *)(uintptr_t)attr->license
				  << "`" << std::endl;
			// EbpfProgWrapper prog;
			int id = bpftime_progs_create(
				(ebpf_inst *)(uintptr_t)attr->insns,
				(size_t)attr->insn_cnt, attr->prog_name,
				attr->prog_type);
			std::cout << "Loaded program `" << attr->prog_name
				  << "` id =" << id << std::endl;
			return id;
		}
	case BPF_LINK_CREATE: {
		auto prog_fd = attr->link_create.prog_fd;
		auto target_fd = attr->link_create.target_fd;
		std::cout << "Creating link " << prog_fd << " -> " << target_fd
			  << std::endl;
		int id = bpftime_link_create(prog_fd, target_fd);
		std::cout << "Created link " << id << std::endl;
		return id;
	}
	case BPF_MAP_FREEZE: {
		// std::cout << "Freezing map" << std::endl;
		// if (auto itr = objs.find(attr->map_fd); itr != objs.end()) {
		// 	if (std::holds_alternative<EbpfMapWrapper>(
		// 		    *itr->second)) {
		// 		auto &map =
		// 			std::get<EbpfMapWrapper>(*itr->second);
		// 		map.frozen = true;
		// 		return 0;
		// 	} else {
		// 		errno = EINVAL;
		// 		return -1;
		// 	}
		// } else {
		// 	errno = ENOENT;
		// 	return -1;
		// }
		// errno = EINVAL;
		return 0;
	}
	case BPF_OBJ_GET_INFO_BY_FD: {
		std::cout << "Getting info by fd" << std::endl;
		bpftime::bpf_map_attr map_attr;
		const char *map_name;
		int map_type;
		int res = bpftime_map_get_info(attr->info.bpf_fd, &map_attr,
					       &map_name, &map_type);
		if (res < 0) {
			errno = res;
			return -1;
		}
		auto ptr = (bpf_map_info *)((uintptr_t)attr->info.info);
		ptr->btf_id = map_attr.btf_id;
		ptr->btf_key_type_id = map_attr.btf_key_type_id;
		ptr->btf_value_type_id = map_attr.btf_value_type_id;
		ptr->type = map_type;
		ptr->value_size = map_attr.value_size;
		ptr->btf_vmlinux_value_type_id =
			map_attr.btf_vmlinux_value_type_id;
		ptr->key_size = map_attr.key_size;
		ptr->id = attr->info.bpf_fd;
		ptr->ifindex = map_attr.ifindex;
		ptr->map_extra = map_attr.map_extra;
		// 		ptr->netns_dev = map.netns_dev;
		// 		ptr->netns_ino = map.netns_ino;
		ptr->max_entries = map_attr.max_ents;
		ptr->map_flags = map_attr.flags;
		strncpy(ptr->name, map_name, sizeof(ptr->name) - 1);
		return 0;
	}
	default:
		return orig_syscall_fn(__NR_bpf, (long)cmd,
				       (long)(uintptr_t)attr, (long)size);
	};
	return 0;
}

int syscall_context::handle_perfevent(perf_event_attr *attr, pid_t pid, int cpu,
				      int group_fd, unsigned long flags)
{
	if (attr->type == determine_uprobe_perf_type()) {
		// NO legacy bpf types
		bool retprobe = attr->config & determine_uprobe_retprobe_bit();
		size_t ref_ctr_off =
			attr->config >> PERF_UPROBE_REF_CTR_OFFSET_SHIFT;
		const char *name = (const char *)(uintptr_t)attr->config1;
		uint64_t offset = attr->config2;
		std::cout << "Creating uprobe " << name << " offset " << offset
			  << " retprobe " << retprobe << " ref_ctr_off "
			  << ref_ctr_off << std::endl;
		int id = bpftime_uprobe_create(pid, name, offset, retprobe,
					       ref_ctr_off);
		std::cout << "Created uprobe " << id << std::endl;
		return id;
	}
	// if (attr->type == PERF_TYPE_TRACEPOINT) {
	// 	auto id = next_fd.fetch_add(1);
	// 	objs.emplace(id, std::make_unique<EbpfObj>(PerfEventWrapper()));
	// 	return id;
	// } else {
	return orig_syscall_fn(__NR_perf_event_open, (uint64_t)(uintptr_t)attr,
			       (uint64_t)pid, (uint64_t)cpu, (uint64_t)group_fd,
			       (uint64_t)flags);
	// }
}

void *syscall_context::handle_mmap64(void *addr, size_t length, int prot,
				     int flags, int fd, off64_t offset)
{
	// if (!manager->is_allocated(fd)) {
	// 	return orig_mmap64_fn(addr, length, prot, flags, fd, offset);
	// }
	// const auto &handler = manager->get_handler(fd);
	// if (!std::holds_alternative<bpftime::bpf_map_handler>(handler)) {
	// 	return orig_mmap64_fn(addr, length, prot, flags, fd, offset);
	// }
	// auto &map = std::get<bpftime::bpf_map_handler>(handler);
	auto ptr = orig_mmap64_fn(addr, length, prot | PROT_WRITE,
				  flags | MAP_ANONYMOUS, -1, 0);
	// if (std::holds_alternative<ArrayMapImpl>(wrapper.impl)) {
	// 	auto &map = std::get<ArrayMapImpl>(wrapper.impl);
	// 	for (size_t i = 0; i < map.size(); i++) {
	// 		std::copy(map[i].begin(), map[i].end(),
	// 			  (uint8_t *)ptr + i * wrapper.value_size);
	// 	}
	// } else if (std::holds_alternative<RingBufMapImpl>(wrapper.impl)) {
	// 	auto impl = std::get<RingBufMapImpl>(wrapper.impl);
	// 	if (prot == (PROT_WRITE | PROT_READ)) {
	// 		impl->consumer_pos = (unsigned long *)ptr;
	// 		std::cout << "Ringbuf " << fd << " writeable ptr "
	// 			  << ptr << std::endl;
	// 		memset(impl->consumer_pos, 0, length);
	// 	} else if (prot == (PROT_READ)) {
	// 		impl->producer_pos = (unsigned long *)ptr;
	// 		impl->data = (uint8_t *)((uintptr_t)impl->producer_pos +
	// 					 getpagesize());
	// 		std::cout << "Ringbuf " << fd << " readonly ptr " << ptr
	// 			  << std::endl;
	// 		memset(impl->producer_pos, 0, length);
	// 	}
	// }
	// else {
	// 	std::cout << "Currently only supports mapping array backed fds "
	// 		  << std::endl;
	// }
	return orig_mmap64_fn(addr, length, prot, flags, fd, offset);
}

int syscall_context::handle_ioctl(int fd, unsigned long req, int data)
{
	int res;
	switch (req) {
	case PERF_EVENT_IOC_ENABLE: {
		std::cout << "Enabling perf event " << fd << std::endl;
		res = bpftime_attach_enable(fd);
		if (res < 0) {
			return orig_ioctl_fn(fd, req, data);
		}
		return res;
	}
	case PERF_EVENT_IOC_SET_BPF: {
		std::cout << "Setting bpf for perf event " << fd << " and bpf "
			  << data << std::endl;
		res = bpftime_attach_perf_to_bpf(fd, data);
		if (res < 0) {
			return orig_ioctl_fn(fd, req, data);
		}
		return res;
	}
	default:
		return orig_ioctl_fn(fd, req, data);
	}
	return 0;
}

int syscall_context::handle_epoll_create1(int flags)
{
	// int fd = next_fd.fetch_add(1);
	// objs.emplace(fd, std::make_unique<EbpfObj>(EpollWrapper()));
	// return fd;
	return -1;
}

int syscall_context::handle_epoll_ctl(int epfd, int op, int fd,
				      epoll_event *evt)
{
	// if (auto itr = objs.find(epfd); itr != objs.end()) {
	// 	std::optional<std::weak_ptr<RingBuffer> > rb_ptr;
	// 	if (auto itr_fd = objs.find(fd); itr_fd != objs.end()) {
	// 		if (std::holds_alternative<EbpfMapWrapper>(
	// 			    *itr_fd->second)) {
	// 			auto &map = std::get<EbpfMapWrapper>(
	// 				*itr_fd->second);
	// 			if (map.type != BPF_MAP_TYPE_RINGBUF) {
	// 				std::cout << "fd " << fd
	// 					  << " is not a ringbuf map"
	// 					  << std::endl;
	// 				errno = EINVAL;

	// 				return -1;
	// 			}
	// 			rb_ptr = std::get<RingBufMapImpl>(map.impl);
	// 		} else {
	// 			std::cout << "fd " << fd << " is not a map"
	// 				  << std::endl;
	// 			errno = EINVAL;
	// 			return -1;
	// 		}
	// 	} else {
	// 		std::cout << "Bad fd: " << fd << std::endl;
	// 		errno = EINVAL;
	// 		return -1;
	// 	}
	// 	if (std::holds_alternative<EpollWrapper>(*itr->second)) {
	// 		auto &ep = std::get<EpollWrapper>(*itr->second);
	// 		if (op == EPOLL_CTL_ADD) {
	// 			ep.rbs.push_back(rb_ptr.value());
	// 			return 0;
	// 		} else {
	// 			std::cout << "Bad epoll op " << op << std::endl;
	// 			errno = EINVAL;
	// 			return -1;
	// 		}
	// 	} else {
	// 		errno = EINVAL;
	// 		return -1;
	// 	}
	// }
	return orig_epoll_ctl_fn(epfd, op, fd, evt);
}

int syscall_context::handle_epoll_wait(int epfd, epoll_event *evt,
				       int maxevents, int timeout)
{
	// if (auto itr = objs.find(epfd); itr != objs.end()) {
	// 	if (std::holds_alternative<EpollWrapper>(*itr->second)) {
	// 		auto &ep = std::get<EpollWrapper>(*itr->second);
	// 		using namespace std::chrono;

	// 		auto start = high_resolution_clock::now();
	// 		int next_id = 0;
	// 		while (next_id < maxevents) {
	// 			auto now = high_resolution_clock::now();
	// 			auto elasped = duration_cast<milliseconds>(
	// 				now - start);
	// 			if (elasped.count() >= 100)
	// 				break;
	// 			int idx = 0;
	// 			for (auto p : ep.rbs) {
	// 				if (auto ptr = p.lock(); ptr) {
	// 					if (ptr->has_data()) {
	// 						auto &next_event =
	// 							evt[next_id++];
	// 						next_event.events =
	// 							EPOLLIN;
	// 						next_event.data.fd =
	// 							idx;
	// 					}
	// 				}
	// 				idx++;
	// 			}
	// 		}
	// 		return next_id;
	// 	} else {
	// 		errno = EINVAL;
	// 		return -1;
	// 	}
	// }
	return orig_epoll_wait_fn(epfd, evt, maxevents, timeout);
}