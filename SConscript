
import sbms

Import('*')

subdirs = ['genr8', 'genr8_2_hddm', 'HDGeant', 'mcsmear', 'gamp2hddm', 'bggen', 'gen_2pi', 'gen_3pi', 'gen_pi0']

SConscript(dirs=subdirs, exports='env osname', duplicate=0)

sbms.OptionallyBuild(env, ['genphoton', 'genpi', 'gen_2mu', 'genEtaRegge'])
