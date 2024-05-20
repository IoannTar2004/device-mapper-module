#include <linux/device-mapper.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/kobject.h>
#include <linux/average.h>

DECLARE_EWMA(avg_size, 4, 4);

struct my_dmp_dev {
    struct dm_dev* dev;
};

static struct kobject* stat;

static struct statistics {
    unsigned long long read_req;
    unsigned long long write_req;    
    unsigned long long total_req;
    struct ewma_avg_size read_avg_blocksize;
    struct ewma_avg_size write_avg_blocksize;
    struct ewma_avg_size total_avg_blocksize;
} dmp_stats = {0};

/* Конструктор. Вызывается при использовании типа dmp при создании блочного устройства
то есть когда выполняем команду dmsetup create.
*/
static int dmp_ctr(struct dm_target* ti, unsigned int argc, char** argv) {
    struct my_dmp_dev* mpd;

    if (argc != 1) {
        printk(KERN_CRIT "\n Invalid argument count\n");
        ti->error = "Invalid argument count";
        return -EINVAL;
    }

    mpd = kmalloc(sizeof(struct my_dmp_dev), GFP_KERNEL);
    if(mpd == NULL) {
        printk(KERN_CRIT "\nCannot alloc memory!\n");
        ti->error = "Cannot alloc memory!";
        return -ENOMEM;
    }
   
    int result = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &mpd->dev);
    if (result) {
        printk(KERN_CRIT "\nDevice lookup failed!\n");
        kfree(mpd);
        ti->error = "dmp: Device lookup failed";
        return result;
    }

    ti->private = mpd;
    return 0;
}

// Деконструктор. Вызывается при удалении блочного устройства или модуля.

static void device_mapper_proxy_dtr(struct dm_target* ti) {
    struct my_dmp_dev* dmpd = (struct my_dmp_dev*) ti->private;      
    dm_put_device(ti, dmpd->dev);
    kfree(dmpd);              
}

// Данная функция вызывается, когда требуется записать или прочитать данные из блока

static int dmp_map(struct dm_target* ti, struct bio* bio) {
    // общая статистика запросов
    dmp_stats.total_req++;
    ewma_avg_size_add(&dmp_stats.total_avg_blocksize, bio->bi_iter.bi_size);

    // проверка типа запроса: на чтение или на запись
    if (bio_op(bio) == REQ_OP_READ) {
        dmp_stats.read_req++;
        ewma_avg_size_add(&dmp_stats.read_avg_blocksize, bio->bi_iter.bi_size);
    } else if (bio_op(bio) == REQ_OP_WRITE) {
        dmp_stats.write_req++;
        ewma_avg_size_add(&dmp_stats.write_avg_blocksize, bio->bi_iter.bi_size);
    } else
        return DM_MAPIO_KILL;

    //сохранение состояния
    struct my_dmp_dev* mpd = (struct my_dmp_dev*)ti->private;
    bio_set_dev(bio, mpd->dev->bdev);
    submit_bio(bio);
    printk(KERN_INFO "\nMapping Success!\n");
   
    return DM_MAPIO_SUBMITTED;
}

// Вывод статистики sysfs

static ssize_t volumes_show(struct kobject* kobj, struct kobj_attribute* attr, char* buf) {
    return snprintf(buf, PAGE_SIZE, "read:\n reqs: %llu\n avg size: %lu\nwrite:\n \
reqs: %llu\n avg size: %lu\ntotal:\n reqs: %llu\n avg size: %lu\n",
        dmp_stats.read_req,
        (unsigned long) ewma_avg_size_read(&dmp_stats.read_avg_blocksize),
        dmp_stats.write_req,
        (unsigned long) ewma_avg_size_read(&dmp_stats.write_avg_blocksize),
        dmp_stats.total_req,
        (unsigned long) ewma_avg_size_read(&dmp_stats.total_avg_blocksize));
}

static struct kobj_attribute volumes_attr = __ATTR(volumes, 0644, volumes_show, NULL);

static struct target_type device_mapper_proxy = {    
    .name = "dmp",
    .version = {1,0,0},
    .module = THIS_MODULE,
    .ctr = dmp_ctr,
    .dtr = device_mapper_proxy_dtr,
    .map = dmp_map,
};

static int device_mapper_proxy_init(void) {
    // регистрируем цель device mapper
    int result = dm_register_target(&device_mapper_proxy);

    if (result < 0) {
        printk(KERN_CRIT "\nRegister failed %d\n", result);
        return result;
    }

    // настройка ведения статистики
    stat = kobject_create_and_add("stat", &THIS_MODULE->mkobj.kobj);
    if(!stat) {
        printk(KERN_CRIT "\nFailed to add kobject!\n");
        dm_unregister_target(&device_mapper_proxy);
        return -ENOMEM;
    }    
   
    result = sysfs_create_file(stat, &volumes_attr.attr);
    if (result < 0) {
        printk(KERN_CRIT "\nFailed to create sysfs entry!\n");
        dm_unregister_target(&device_mapper_proxy);
        kobject_put(stat);
        return result;
    }

    ewma_avg_size_init(&dmp_stats.read_avg_blocksize);
    ewma_avg_size_init(&dmp_stats.write_avg_blocksize);
    ewma_avg_size_init(&dmp_stats.total_avg_blocksize);

    return 0;
}

static void device_mapper_proxy_exit(void) {
    dm_unregister_target(&device_mapper_proxy);
    kobject_put(stat);
}

module_init(device_mapper_proxy_init);
module_exit(device_mapper_proxy_exit);

MODULE_AUTHOR("Ivan_Tarasov");
MODULE_DESCRIPTION("Proxy device for mapping");
MODULE_LICENSE("GPL");