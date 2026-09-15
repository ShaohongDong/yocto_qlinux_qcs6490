#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute the actual driver's pre-MMIO probe and reset code with host stubs.

Checks completion initialization for each child mask. No MMIO, IRQ timing or
hardware correctness is simulated. Unmodified master1-only probe must fail.
"""

import argparse
import subprocess
import tempfile
from pathlib import Path

STUBS = r"""
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
typedef unsigned u32;
#define NUM_MASTERS 2
#define I2C_MODE_STANDARD 0
#define I2C_MODE_FAST 1
#define I2C_MODE_FAST_PLUS 2
#define I2C_MAX_FAST_MODE_FREQ 400000
#define I2C_MAX_FAST_MODE_PLUS_FREQ 1000000
#define CCI_RESET_CMD 0
#define CCI_RESET_CMD_MASK 0
#define CCI_TIMEOUT 1
#define ETIMEDOUT 110
#define dev_err(...) ((void)0)
struct completion { bool initialized; unsigned done; };
static void init_completion(struct completion *c) { c->initialized=true; c->done=0; }
static void reinit_completion(struct completion *c) { c->done=0; }
static int wait_for_completion_timeout(struct completion *c,int t) { return c->initialized; }
#define writel(...) ((void)0)
struct device_node { unsigned reg; bool available; };
struct device { struct device_node *of_node; };
struct adapter { const void *quirks,*algo; struct { void *parent; struct device_node *of_node; } dev; char name[64]; };
struct cci_master { struct adapter adap; int master,mode; void *cci; struct completion irq_complete; };
struct data { unsigned num_masters; int quirks; };
struct cci { const struct data *data; struct cci_master master[2]; struct device *dev; char *base; };
static int cci_algo;
#define for_each_available_child_of_node(root, child) for(child=(root);child<(root)+2;child++) if(child->available)
static int of_property_read_u32(struct device_node *n,const char *p,u32 *v) { if(strcmp(p,"reg"))return -1;*v=n->reg;return 0; }
#define of_node_get(n) (n)
#define i2c_set_adapdata(a,m) ((void)0)
"""


def check(source):
    text = Path(source).read_text()
    probe = text.split("static int cci_probe(", 1)[1]
    preparation = probe.split("return -ENOENT;", 1)[1].split("/* Memory */", 1)[0]
    reset = text[
        text.index("static int cci_reset(") : text.index("static int cci_init(")
    ]
    program = (
        STUBS
        + "\n"
        + reset
        + "\nstatic void prepare(struct cci *cci,struct device *dev) {struct device_node *child;int ret,i;u32 val;\n"
        + preparation
        + "\n}\n"
    )
    program += r"""
int main(void) {
 int failed=0;
 for(unsigned mask=1;mask<4;mask++) {
  struct data data={.num_masters=2};
  struct device_node children[2]={{0,mask&1},{1,mask&2}};
  struct device dev={children};struct cci c={.data=&data,.dev=&dev};
  prepare(&c,&dev);
  int ok=cci_reset(&c)==0;
  printf("children=%u reset_completion_initialized=%s\n",mask,ok?"yes":"NO");
  if(!ok)failed=1;
 }
 return failed;
}
"""
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / "check.c").write_text(program)
        subprocess.run(
            ["cc", "-std=c11", "-o", str(root / "check"), str(root / "check.c")],
            check=True,
        )
        return subprocess.run([str(root / "check")], check=False).returncode


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    raise SystemExit(check(parser.parse_args().driver))
