/* Nachbildung der Arbeitswarteschlange: der Test ruft die Funktion selbst. */
#ifndef _FAKE_WORKQUEUE_H
#define _FAKE_WORKQUEUE_H

struct work_struct {
	void (*func)(struct work_struct *w);
};

struct delayed_work {
	struct work_struct work;
};

#define INIT_DELAYED_WORK(dw, fn)	((dw)->work.func = (fn))

/*
 * Einreihen und Abbrechen zaehlen nur mit - abgearbeitet wird im Test
 * durch einen direkten Aufruf, damit jeder Durchgang nachvollziehbar
 * bleibt.
 */
extern unsigned long fake_wq_queued;
extern unsigned long fake_wq_cancelled;

static inline int schedule_delayed_work(struct delayed_work *dw,
					unsigned long delay)
{
	(void)dw;
	(void)delay;
	fake_wq_queued++;
	return 1;
}

static inline int cancel_delayed_work_sync(struct delayed_work *dw)
{
	(void)dw;
	fake_wq_cancelled++;
	return 0;
}

#endif
