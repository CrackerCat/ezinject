/*
 * Copyright (C) 2021 Stefano Moioli <smxdev4@gmail.com>
 * This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 *  1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 */

#include <stdio.h>
#include <dlfcn.h>
#include <signal.h>
#include "ezinject_common.h"

/**
 * a no-op program that is used to bootstrap libc
 */
int main(int argc, char *argv[]){
	UNUSED(argc);
	UNUSED(argv);
	puts("HELLO WORLD");

	// just to ensure libdl is linked in
	void *self = dlopen(NULL, RTLD_LAZY);
	if(self != NULL){
		dlclose(self);
	}

	// time for ezinject to cleanup
	raise(SIGTRAP);
	return 0;
}