fiber = require('fiber')
fio = require('fio')

-- Temporary space for bumping lsn.
temp = box.schema.space.create('temp')
_ = temp:create_index('pk')

s = box.schema.space.create('test', {engine='vinyl'})
_ = s:create_index('pk', {run_count_per_level=1})

path = fio.pathjoin(box.cfg.vinyl_dir, tostring(s.id), tostring(s.index.pk.id))

function file_count() return #fio.glob(fio.pathjoin(path, '*')) end
function vylog_count() return #fio.glob(fio.pathjoin(box.cfg.vinyl_dir, '*.vylog')) end
function gc() temp:auto_increment{} box.snapshot() box.internal.gc.run(box.info.signature) end

-- Check that run files are deleted by gc.
s:insert{1} box.snapshot() -- dump
s:insert{2} box.snapshot() -- dump + compaction
while s.index.pk:info().run_count > 1 do fiber.sleep(0.01) end -- wait for compaction
file_count()
gc()
file_count()

-- Check that gc keeps the current and previous log files.
vylog_count()

-- Check that files left from dropped indexes are deleted by gc.
s:drop()
gc()
file_count()

--
-- Check that vylog files are removed if vinyl is not used.
--

vylog_count()

-- All records should have been purged from the log by now
-- so we should only keep the previous log file.
gc()
vylog_count()

-- The previous log file should be removed by the next gc.
gc()
vylog_count()

temp:drop()
