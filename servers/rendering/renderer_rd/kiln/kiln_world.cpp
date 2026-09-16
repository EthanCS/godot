// Kiln engine integration. Engine licensing: LICENSE.txt.
// Imported algorithm provenance and redistribution limits: kiln/docs/gi-provenance.json.

#include "kiln_world.h"

namespace RendererRD {
static Mutex kiln_world_mutex;
static HashMap<RID, KilnWorld> kiln_worlds;
static HashMap<RID, Dictionary> kiln_statistics;
void KilnWorld::report(RID p_environment, const Dictionary &p_statistics) {
	MutexLock lock(kiln_world_mutex);
	kiln_statistics.insert(p_environment, p_statistics);
}
Dictionary KilnWorld::statistics(RID p_environment) {
	MutexLock lock(kiln_world_mutex);
	const Dictionary *data = kiln_statistics.getptr(p_environment);
	return data ? *data : Dictionary();
}

void KilnWorld::publish(RID p_environment, const KilnWorld &p_world) {
	MutexLock lock(kiln_world_mutex);
	kiln_worlds.insert(p_environment, p_world);
}
bool KilnWorld::read(RID p_environment, KilnWorld &r_world) {
	MutexLock lock(kiln_world_mutex);
	const KilnWorld *world = kiln_worlds.getptr(p_environment);
	if (!world) {
		return false;
	}
	r_world = *world;
	return true;
}
void KilnWorld::remove(RID p_environment) {
	MutexLock lock(kiln_world_mutex);
	kiln_worlds.erase(p_environment);
	kiln_statistics.erase(p_environment);
}
} //namespace RendererRD
