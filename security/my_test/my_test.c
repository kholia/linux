#include <linux/lsm_hooks.h>
#include <linux/kern_levels.h>
#include <linux/binfmts.h>

static int my_test_bprm_check_security (struct linux_binprm *bprm)
{
	printk(KERN_ERR "Hello from my_test_bprm_check_security: %s\n", bprm->interp);

	return 0;
}

static int my_test_file_open(struct file *file)
{
	printk(KERN_ERR "Hello from my_test_file_open: %s\n", file->f_path.dentry->d_name.name);

	return 0;
}

const struct lsm_id my_test_lsmid = {
	.name = "my_test",
	.id = LSM_ID_MY_TEST,
};

static struct security_hook_list my_test_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(bprm_check_security, my_test_bprm_check_security),
	LSM_HOOK_INIT(file_open, my_test_file_open),
};

static __init int my_test_init(void)
{
	printk(KERN_ERR "mytest: We are going to do things!\n");
	security_add_hooks(my_test_hooks, ARRAY_SIZE(my_test_hooks), &my_test_lsmid);

	return 0;
}

DEFINE_LSM(my_test) = {
	.name = "my_test",
	.init = my_test_init,
};
