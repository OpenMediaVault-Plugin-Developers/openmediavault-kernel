<?php
/**
 * @license   http://www.gnu.org/licenses/gpl.html GPL Version 3
 * @author    OpenMediaVault Plugin Developers <plugins@omv-extras.org>
 * @copyright Copyright (c) 2021-2026 openmediavault plugin developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

class OMVRpcServiceKernel extends \OMV\Rpc\ServiceAbstract
{
    public function getName()
    {
        return 'kernel';
    }

    public function initialize()
    {
        $this->registerMethod('getArch');

        $this->registerMethod('setKernel');
        $this->registerMethod('unsetKernel');
        $this->registerMethod('getFamilyList');
        $this->registerMethod('setFamily');
        $this->registerMethod('getKernelList');
        $this->registerMethod('deleteKernel');

        $this->registerMethod('doCommand');
        $this->registerMethod('doProxmox');
        $this->registerMethod('doMemtest');
        $this->registerMethod('doDebian');
        $this->registerMethod('doUpdateGrub');

        $this->registerMethod('getIsoList');
        $this->registerMethod('writeIso');
    }

    public function getArch($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // Get architecture
        $object = [];
        $cmd = new \OMV\System\Process('dpkg --print-architecture');
        $cmd->execute($output);
        $object['arch'] = strtolower($output[0]);
        // Return object
        return $object;
    }

    public function setKernel($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);

        $key = trim($params['key']);
        $bootOnce = $params['boot_once'];

        // Map key -> entry_id using the parsed grub entries.
        $entries = $this->parseGrubEntries('/boot/grub/grub.cfg');
        $match = null;
        foreach ($entries as $e) {
            if ($e['title'] === $key) {
                $match = $e;
                break;
            }
        }
        if ($match === null) {
            throw new \OMV\ExecException(sprintf('Kernel/entry not found for key: %s', $key));
        }

        $entryId = $match['entry_id'];
        if (!$entryId) $entryId = sprintf("'%s'", $key);

        $updateGrub = false;
        $grubDefaultPath = '/etc/default/grub';
        $content = file_get_contents($grubDefaultPath);
        if (!preg_match('/^GRUB_DEFAULT=/m', $content)) {
            $content .= PHP_EOL . 'GRUB_DEFAULT=saved' . PHP_EOL;
            file_put_contents($grubDefaultPath, $content);
            $updateGrub = true;
        } elseif (!preg_match('/^GRUB_DEFAULT\s*=\s*saved\s*$/m', $content)) {
            $new = preg_replace('/^GRUB_DEFAULT\s*=.*$/m', 'GRUB_DEFAULT=saved', $content);
            if ($new !== null && $new !== $content) {
                file_put_contents($grubDefaultPath, $new);
                $updateGrub = true;
            }
        }
        // A persistent pin replaces any kernel family selection.
        if (!$bootOnce && $this->unsetFamily()) {
            $updateGrub = true;
        }
        if ($updateGrub) {
            $this->runUpdateGrub();
        }

        if ($bootOnce) {
            $cmd = new \OMV\System\Process('grub-reboot', $entryId);
        } else {
            $cmd = new \OMV\System\Process('grub-set-default', $entryId);
        }
        $cmd->execute($output);
        return [
            'entry_id' => $entryId,
            'title' => $match['title'],
            'kernel' => $match['kernel'],
            'boot_once' => $bootOnce
        ];
    }

    public function unsetKernel($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);

        $this->unsetDefaultKernel();

        return [];
    }

    // Reset the GRUB default so it boots the first menu entry (typically the
    // newest installed kernel) and clear any saved/boot-once selection. Shared
    // by the unset RPC and the kernel install paths.
    private function unsetDefaultKernel($bgOutputFilename = null)
    {
        // Reset GRUB_DEFAULT to 0 so GRUB boots the first entry it finds.
        $updateGrub = $this->unsetFamily();
        $grubDefaultPath = '/etc/default/grub';
        $content = file_get_contents($grubDefaultPath);
        $new = preg_replace('/^GRUB_DEFAULT\s*=.*$/m', 'GRUB_DEFAULT=0', $content);
        if ($new !== null && $new !== $content) {
            file_put_contents($grubDefaultPath, $new);
            $updateGrub = true;
        }
        if ($updateGrub) {
            $this->runUpdateGrub($bgOutputFilename);
        }

        // Clear saved_entry and next_entry from grubenv.
        $cmd = new \OMV\System\Process('grub-editenv', ['/boot/grub/grubenv', 'unset', 'saved_entry', 'next_entry']);
        $cmd->execute($output);
    }

    // List the kernel families that can be selected, built from the installed
    // kernels. See omv-kernelfamily for how a family is resolved.
    public function getFamilyList($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);

        $current = $this->getFamily();
        $series = [];
        foreach (glob('/boot/vmlinuz-*') ?: [] as $path) {
            $kver = substr(basename($path), strlen('vmlinuz-'));
            if (preg_match('/\.(old|dpkg-.*)$/', $kver)) {
                continue;
            }
            if (!preg_match('/^(\d+\.\d+)\./', $kver, $m)) {
                continue;
            }
            $distro = str_ends_with($kver, '-pve') ? 'pve' : 'debian';
            $series[$distro][$m[1]] = true;
        }

        $families = [];
        foreach (['pve', 'debian'] as $distro) {
            if (empty($series[$distro])) {
                continue;
            }
            $ids = [$distro];
            $versions = array_keys($series[$distro]);
            usort($versions, 'version_compare');
            foreach (array_reverse($versions) as $ver) {
                $ids[] = sprintf('%s-%s', $distro, $ver);
            }
            foreach ($ids as $id) {
                $families[] = ['id' => $id, 'name' => $this->getFamilyLabel($id)];
            }
        }
        foreach ($families as &$family) {
            if ($family['id'] === $current) {
                $family['name'] = sprintf('%s (%s)', $family['name'], gettext('current'));
            }
        }
        unset($family);
        return $families;
    }

    public function setFamily($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, 'rpc.kernel.setfamily');
        // Create the background process.
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
            use ($params) {
                $cmd = new \OMV\System\Process('omv-kernelfamily', ['set', $params['family']]);
                $cmd->setRedirect2to1();
                $cmdLine = $cmd->getCommandLine();
                if (0 !== $this->exec($cmdLine, $output, $bgOutputFilename))
                    throw new \OMV\ExecException($cmdLine, $output);
                $this->runUpdateGrub($bgOutputFilename);
                return $output;
            }
        );
    }

    private function getFamilyLabel($family)
    {
        if (!preg_match('/^(pve|debian)(?:-(\d+\.\d+))?$/', $family, $m)) {
            return $family;
        }
        $label = ($m[1] === 'pve') ? 'Proxmox' : 'Debian';
        if (empty($m[2])) {
            return sprintf('%s (%s)', $label, gettext('latest'));
        }
        return sprintf('%s %s', $label, $m[2]);
    }

    // Describe the active kernel family, noting when a series family has no
    // installed kernels and falls back to the newest of its distribution.
    private function getFamilyReason($family)
    {
        $reason = sprintf('%s: %s', gettext('Family'), $this->getFamilyLabel($family));
        if (!preg_match('/^(pve|debian)-(\d+\.\d+)$/', $family, $m)) {
            return $reason;
        }
        $output = [];
        try {
            $cmd = new \OMV\System\Process('omv-kernelfamily', ['resolve', $family]);
            $cmd->execute($output);
        } catch (\Exception $e) {
            return $reason;
        }
        $kver = substr(basename(trim($output[0] ?? '')), strlen('vmlinuz-'));
        if ($kver === '' || !str_starts_with($kver, $m[2] . '.')) {
            $reason .= sprintf(' → %s %s', gettext('fallback'), $this->getFamilyLabel($m[1]));
        }
        return $reason;
    }

    private function getGrubDefault()
    {
        $content = @file_get_contents('/etc/default/grub');
        if ($content === false || !preg_match_all('/^GRUB_DEFAULT\s*=\s*(.*)$/m', $content, $m)) {
            return null;
        }
        // The last assignment wins, as when the file is sourced.
        return trim(end($m[1]), " \t\"'");
    }

    private function getFamily()
    {
        $output = [];
        try {
            $cmd = new \OMV\System\Process('omv-kernelfamily', ['get']);
            $cmd->execute($output);
        } catch (\Exception $e) {
            return null;
        }
        $family = trim($output[0] ?? '');
        return $family !== '' ? $family : null;
    }

    // Remove the kernel family selection. Returns true if one was removed,
    // in which case update-grub must be run.
    private function unsetFamily()
    {
        if ($this->getFamily() === null) {
            return false;
        }
        $cmd = new \OMV\System\Process('omv-kernelfamily', ['unset']);
        $cmd->execute($output);
        return true;
    }

    private function runUpdateGrub($bgOutputFilename = null)
    {
        $cmd = new \OMV\System\Process('update-grub');
        if ($bgOutputFilename === null) {
            $cmd->execute($output);
        } else {
            $cmd->setRedirect2to1();
            $cmdLine = $cmd->getCommandLine();
            if (0 !== $this->exec($cmdLine, $output, $bgOutputFilename))
                throw new \OMV\ExecException($cmdLine, $output);
        }
    }

    private function getCurrentKernel()
    {
        $current = php_uname('r');
        return is_string($current) && $current !== '' ? $current : null;
    }

    private function parseGrubEntries()
    {
        $grubCfg = '/boot/grub/grub.cfg';
        if (!is_readable($grubCfg)) {
            return [];
        }

        $lines = file($grubCfg);
        $entries = [];

        foreach ($lines as $line) {
            $title = null;
            $id = null;
            $kernel = null;

            $trimmed = trim($line);
            $outer = '/^\s*menuentry\s+(["\'])(?P<title>.*?)\1(?:.*?\$menuentry_id_option\s+(["\'])(?P<id>.*?)\3)?\s*\{/';
            if (preg_match($outer, $trimmed, $m)) {
                $title = $m['title'];
                $id = $m['id'] ?? null;
                $kernel = null;
                if (preg_match('/\bLinux\s+(?P<kernel>\d+(?:\.\d+)+(?:[^\s\'")]+)?)/', $title, $km)) {
                    $kernel = $km['kernel'];
                }
            } else {
                continue;
            }
            if (!empty($title)) {
                $entries[] = [
                    'title' => $title,
                    'kernel' => $kernel,
                    'entry_id' => $id
                ];
            }
        }
        return $entries;
    }

    public function getKernelList($params, $context)
    {
        // Validate the RPC caller context
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);

        $grubCfg = '/boot/grub/grub.cfg';
        $entries = $this->parseGrubEntries($grubCfg);
        $savedEntry = $this->getGrubEnvValue('saved_entry');
        $nextEntry = $this->getGrubEnvValue('next_entry');
        $currentKernel = $this->getCurrentKernel();
        $family = $this->getFamily();
        $grubDefault = $this->getGrubDefault();

        // Is saved_entry set, and does it still match a menu entry? GRUB boots
        // the first entry when the saved entry no longer exists.
        $savedSet = is_string($savedEntry) && $savedEntry !== '' && $savedEntry !== 'saved';
        $savedValid = false;
        if ($savedSet) {
            foreach ($entries as $e) {
                if ($savedEntry === $e['entry_id'] || $savedEntry === $e['title']) {
                    $savedValid = true;
                    break;
                }
            }
        }

        // Explain why the default kernel boots.
        if ($savedValid) {
            $defaultReason = gettext('Set boot');
        } elseif ($family !== null) {
            $defaultReason = $this->getFamilyReason($family);
        } elseif ($savedSet) {
            $defaultReason = gettext('GRUB fallback (Set boot kernel missing)');
        } elseif ($grubDefault === '0' || $grubDefault === 'saved') {
            $defaultReason = gettext('GRUB fallback (highest version)');
        } else {
            $defaultReason = sprintf('GRUB_DEFAULT=%s', $grubDefault ?? '');
        }

        $kernels = [];
        $foundDefault = false;
        foreach ($entries as $e) {
            $entryId = $e['entry_id'];
            $name = $e['title'];
            $kver = $e['kernel'];

            // Determine default:
            // Prefer a saved_entry that still matches a menu entry. Otherwise
            // GRUB boots the first entry when GRUB_DEFAULT is 0 or saved (a
            // kernel family moves its kernel to the top). For any other
            // GRUB_DEFAULT, fall back to matching the running kernel version.
            $isDefault = false;
            if ($savedValid) {
                $isDefault = ($savedEntry === $entryId || $savedEntry === $name);
            } elseif ($family !== null || $savedSet || $grubDefault === '0' || $grubDefault === 'saved') {
                $isDefault = !$foundDefault;
            } elseif (!$foundDefault && !empty($currentKernel) && !empty($kver)) {
                $isDefault = str_starts_with($currentKernel, $kver);
            }
            if ($isDefault) {
                $foundDefault = true;
            }

            $isNext = (is_string($nextEntry) && $nextEntry !== '' && ($nextEntry === $entryId || $nextEntry === $name));

            $remove = false;
            if (!$isDefault) {
                $hasDebian = strpos($name, 'Debian GNU/Linux, with Linux') !== false;
                $hasProxmox = strpos($name, 'Proxmox Virtual Environment GNU/Linux, with Linux') !== false;
                $isRecovery = strpos($name, 'recovery mode') !== false;
                if (!$isRecovery && ($hasDebian || $hasProxmox)) {
                    $remove = true;
                }
            }

            $kfamily = '';
            if (!empty($kver) && preg_match('/^(\d+\.\d+)\./', $kver, $m)) {
                $kfamily = sprintf('%s %s', str_ends_with($kver, '-pve') ? 'Proxmox' : 'Debian', $m[1]);
            }

            $kernels[] = [
                'name' => $name,
                'key' => $kver,
                'family' => $kfamily,
                'entry_id' => $entryId,
                'default' => $isDefault,
                'reason' => $isDefault ? $defaultReason : '',
                'nextk' => $isNext,
                'remove' => $remove
            ];
        }

        return $this->applyFilter($kernels, $params['start'], $params['limit'],
            $params['sortfield'], $params['sortdir']);
    }

    private function getGrubEnvValue($key)
    {
        $output = [];
        try {
            $cmd = new \OMV\System\Process('grub-editenv', ['list']);
            $cmd->execute($output);
        } catch (\Exception $e) {
            return null;
        }
        foreach ($output as $line) {
            if (preg_match('/^' . preg_quote($key, '/') . '=(.*)$/', $line, $m)) {
                return trim($m[1]);
            }
        }
        return null;
    }

    public function deleteKernel($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // Check if the package database is locked.
        \OMV\System\Apt::assertNotLocked();
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
          use ($params) {
            // Prefer the pre-extracted 'key' (kernel version), fall back to parsing 'name'.
            $kver = !empty($params['key']) ? trim($params['key']) : '';
            if (empty($kver)) {
                $parts = explode(' ', $params['name']);
                $kver = isset($parts[4]) ? trim($parts[4]) : '';
            }
            if (empty($kver)) {
                throw new \OMV\Exception("Could not determine kernel version from: %s", $params['name']);
            }

            // Get all installed packages and filter in PHP — avoids shell expansion
            // issues with format strings and glob patterns containing '+' characters.
            $allPkgs = [];
            $cmd = new \OMV\System\Process('dpkg', ['--get-selections']);
            $cmd->execute($allPkgs);

            $packages = [];
            foreach ($allPkgs as $line) {
                $parts = preg_split('/\s+/', trim($line), 2);
                $pkg = trim($parts[0] ?? '');
                $status = trim($parts[1] ?? '');
                if (empty($pkg) || $status !== 'install') continue;
                if (!preg_match('/^(linux-image|linux-headers|linux-base|proxmox-kernel|pve-kernel)/', $pkg)) continue;
                if (strpos($pkg, $kver) === false) continue;
                $packages[] = $pkg;
            }
            $packages = array_unique($packages);

            if (empty($packages)) {
                throw new \OMV\Exception("No installed packages found for kernel version: %s", $kver);
            }

            // Purge the packages.
            $cmdArgs = array_merge(['--yes', '--autoremove', 'purge'], $packages);
            $cmd = new \OMV\System\Process('apt-get', $cmdArgs);
            $cmd->setRedirect2to1();
            $cmd->setEnv("DEBIAN_FRONTEND", "noninteractive");
            if (0 !== ($exitStatus = $this->exec($cmd->getCommandLine(), $output, $bgOutputFilename))) {
                throw new \OMV\ExecException($cmd->getCommandLine(), $output, $exitStatus);
            }

            // Regenerate grub config so removed kernels no longer appear in the menu.
            $this->runUpdateGrub($bgOutputFilename);

            return $output;
        });
    }

    public function doCommand($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, 'rpc.kernel.docommand');
        // Create the background process.
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
            use ($params) {
                $cmdParams = [];
                $cmdParams[] = '/usr/sbin/omv-installiso';
                $cmdParams[] = $params['action'];
                $cmdParams[] = $params['distro'];
                $cmd = new \OMV\System\Process($cmdParams);
                $cmdLine = $cmd->getCommandLine();
                if (0 !== $this->exec($cmdLine, $output, $bgOutputFilename))
                    throw new \OMV\ExecException($cmdLine, $output);
                return $output;
            }
        );
    }

    public function doProxmox($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, 'rpc.kernel.doproxmox');
        // Create the background process.
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
            use ($params) {
                $cmdArgs = [];
                if ($params['command'] == 'remove') {
                    $cmdBin = 'omv-removekernels';
                } else {
                    // Clear any previous default/boot-once selection first; the
                    // installer then selects the installed Proxmox series as the
                    // boot kernel family.
                    $this->unsetDefaultKernel($bgOutputFilename);
                    $cmdBin = 'omv-installproxmox';
                    $cmdArgs[] = $params['version'];
                }
                $cmd = new \OMV\System\Process($cmdBin, $cmdArgs);
                $cmdLine = $cmd->getCommandLine();
                if (0 !== $this->exec($cmdLine, $output, $bgOutputFilename))
                    throw new \OMV\ExecException($cmdLine, $output);
                return $output;
            }
        );
    }

    public function doMemtest($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, 'rpc.kernel.domemtest');
        // Create the background process.
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
            use ($params) {
                $cmdParams = [];
                $cmdParams[] = 'apt-get';
                $cmdParams[] = '-y';
                $cmdParams[] = $params['action'];
                $cmdParams[] = 'memtest86+';
                $cmd = new \OMV\System\Process($cmdParams);
                $cmdLine = $cmd->getCommandLine();
                if (0 !== $this->exec($cmdLine, $output, $bgOutputFilename))
                    throw new \OMV\ExecException($cmdLine, $output);
                return $output;
            }
        );
    }

    public function doDebian($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, 'rpc.kernel.dodebian');
        // Check if the package database is locked.
        \OMV\System\Apt::assertNotLocked();
        // Create the background process.
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
            use ($params) {
            $package = $params['package'];
            $cmd = new \OMV\System\Process('apt-get', [
                '--yes', 'install', '--reinstall', $package
            ]);
            $cmd->setRedirect2to1();
            $cmd->setEnv("DEBIAN_FRONTEND", "noninteractive");
            if (0 !== ($exitStatus = $this->exec($cmd->getCommandLine(), $output, $bgOutputFilename))) {
                throw new \OMV\ExecException($cmd->getCommandLine(), $output, $exitStatus);
            }
            if ($package === 'linux-image-amd64') {
                $this->unsetDefaultKernel($bgOutputFilename);
                $this->runUpdateGrub($bgOutputFilename);
            }
            return $output;
        });
    }

    public function doUpdateGrub($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // Create the background process.
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename) {
            $this->runUpdateGrub($bgOutputFilename);
            return [];
        });
    }

    public function getIsoList($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);

        $isos = [];
        $output = [];
        try {
            $cmd = new \OMV\System\Process('find', ['/boot', '-maxdepth', '3',
                '-type', 'f', '-iname', '*.iso']);
            $cmd->execute($output);
        } catch (\Exception $e) {
            $output = [];
        }
        foreach ($output as $path) {
            $path = trim($path);
            if ($path === '' || !is_file($path)) {
                continue;
            }
            $isos[] = [
                'path' => $path,
                'name' => basename($path)
            ];
        }
        return $isos;
    }

    public function writeIso($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, 'rpc.kernel.dowriteiso');
        // Create the background process.
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
            use ($params) {
                $cmd = new \OMV\System\Process('omv-writeiso',
                    [$params['iso'], $params['device']]);
                $cmd->setRedirect2to1();
                $cmdLine = $cmd->getCommandLine();
                if (0 !== $this->exec($cmdLine, $output, $bgOutputFilename))
                    throw new \OMV\ExecException($cmdLine, $output);
                return $output;
            }
        );
    }
}
