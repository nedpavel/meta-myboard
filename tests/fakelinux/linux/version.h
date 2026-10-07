/*
 * Kernelversion fuer das Abspielwerk - erzwingbar.
 *
 * Der Treiber waehlt an drei Stellen per LINUX_VERSION_CODE zwischen der
 * Schreibweise fuer 5.10 und der fuer 6.x (vm_flags, no_llseek,
 * class_create). Damit auch der 6.x-Zweig uebersetzt und nicht nur
 * hingeschrieben ist, laesst sich die Version hier ueber die
 * Befehlszeile setzen:
 *
 *   make check        5.10.16  - wie der Herstellerkernel
 *   make check-6x     6.12.0   - wie ein Wrynose-Kernel
 *
 * Geprueft wird damit, dass beide Zweige syntaktisch aufgehen und
 * dieselben Pruefungen bestehen. Ob der echte 6.x-Kernel noch etwas
 * beanstandet, zeigt erst der Bau gegen seine Header.
 */
#ifndef _FAKE_LINUX_VERSION_H
#define _FAKE_LINUX_VERSION_H

#define KERNEL_VERSION(a, b, c)		(((a) << 16) + ((b) << 8) + (c))

#ifndef FAKE_KERNEL_CODE
#define FAKE_KERNEL_CODE		KERNEL_VERSION(5, 10, 16)
#endif

#define LINUX_VERSION_CODE		FAKE_KERNEL_CODE

#endif
