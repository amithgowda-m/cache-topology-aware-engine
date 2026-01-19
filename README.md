Here is the updated **README specifically for running this project in a Kali Linux VM**.

Running a kernel module that accesses **CPU Performance Counters (Cache Misses)** inside a Virtual Machine requires specific settings. Standard VMs often "hide" these hardware counters from the guest OS, which will cause the `ctae_monitor` module to fail unless configured correctly.

### `README_KALI_VM.md`

```markdown
# CTAE - Setup Guide for Kali Linux (VM)

This guide explains how to run the CTAE project inside a Kali Linux Virtual Machine.

## 🚨 Critical VM Step (Do this First!)
By default, VirtualBox and VMware **block** the hardware performance counters this project needs to count cache misses. You must enable them on your **Host PC** before starting the VM.

### If using VMware Workstation / Player:
1. Shut down the Kali VM.
2. Go to **Edit Virtual Machine Settings** -> **Processors**.
3. Check the box: **"Virtualize CPU performance counters"**.
4. Start the VM.

### If using VirtualBox:
1. Shut down the Kali VM.
2. Open a command prompt/terminal on your **Host PC** (Windows/Mac/Linux).
3. Run this command (replace `"Kali Linux"` with your exact VM name):
   ```bash
   VBoxManage modifyvm "Kali Linux" --l2cache on --l3cache on

```

*Note: VirtualBox often struggles to pass accurate cache topology to the kernel. VMware is recommended for this specific project.*

---

## 💻 Step 1: Install Kernel Headers in Kali

Kali Linux updates its kernel frequently. You need the headers that match your *current* running kernel exactly.

1. Open the terminal in Kali.
2. Update and install headers:
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

```


*(If this fails, try `sudo apt install linux-headers-amd64`)*

---

## 🚀 Step 2: Build and Run

1. Navigate to the folder:
```bash
cd ctae

```


2. Make the script executable:
```bash
chmod +x run.sh

```


3. Run the build script:
```bash
sudo ./run.sh

```



**✅ Success Indicator:**
You should see: `[7/7] Loaded modules: ... ctae_policy ... ctae_monitor ... ctae_core`

**❌ Failure Indicator:**
If `ctae_monitor` fails to load, it is likely because the VM cannot see the CPU counters. Check `dmesg` (see below).

---

## 🧪 Step 3: Run the Test

1. Compile the stress test:
```bash
gcc -pthread -o stress_test stress_test.c -Wall

```


2. Open a **second terminal** to watch the logs:
```bash
sudo dmesg -w | grep CTAE

```


3. Run the stress test in the **first terminal**:
```bash
sudo ./stress_test

```



---

## 🛠️ Troubleshooting for Kali VMs

**Issue 1: "insmod: ERROR: ... Invalid argument" or "No such device"**

* **Cause:** The VM cannot access hardware performance counters (PMU).
* **Fix:** Ensure you followed the **Critical VM Step** above. If VirtualBox still fails, try enabling "VT-x/AMD-V" in the VM System settings.

**Issue 2: "Make: *** /lib/modules/.../build: No such file or directory"**

* **Cause:** You updated Kali (`apt upgrade`) but haven't rebooted, so the running kernel doesn't match the installed headers.
* **Fix:** Reboot the VM (`sudo reboot`) and try again.

**Issue 3: "storage size of ‘ctae_global_domain’ isn’t known"**

* **Cause:** The compiler isn't finding the local header file.
* **Fix:** Open `core/Makefile` and ensure it looks like this:
```makefile
obj-m := ctae_core.o
ccflags-y := -I$(src)       <-- THIS LINE IS IMPORTANT
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean

```



```

```