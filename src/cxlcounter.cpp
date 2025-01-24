/*
 * CXLMemSim counter
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "cxlcounter.h"

void CXLSwitchEvent::inc_load() { load++; }
void CXLSwitchEvent::inc_store() { store++; }
void CXLSwitchEvent::inc_conflict() { conflict++; }
void CXLMemExpanderEvent::inc_load() { load++; }
void CXLMemExpanderEvent::inc_store() { store++; }
void CXLMemExpanderEvent::inc_migrate() { migrate++; }
void CXLMemExpanderEvent::inc_hit_old() { hit_old++; }
void CXLCounter::inc_local() { local++; }
void CXLCounter::inc_remote() { remote++; }
void CXLCounter::inc_hitm() { hitm++; }
