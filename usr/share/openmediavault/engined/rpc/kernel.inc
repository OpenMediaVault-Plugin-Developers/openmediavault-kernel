<?php
/**
 * @license   http://www.gnu.org/licenses/gpl.html GPL Version 3
 * @author    OpenMediaVault Plugin Developers <plugins@omv-extras.org>
 * @copyright Copyright (c) 2021-2025 openmediavault plugin developers
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
        $this->registerMethod('getKernelList');
        $this->registerMethod('deleteKernel');

        $this->registerMethod('doCommand');
        $this->registerMethod('doProxmox');
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

        // Ensure GRUB is configured to honor saved/next entries.
        $grubDefaultPath = '/etc/default/grub';
        // If GRUB_DEFAULT is not "saved", set it to "saved" (idempotent).
        $content = file_get_contents($grubDefaultPath);

        // Normalize: if there is no GRUB_DEFAULT line, add it; else force to saved.
        if (!preg_match('/^GRUB_DEFAULT=/m', $content)) {
            $content .= PHP_EOL . 'GRUB_DEFAULT=saved' . PHP_EOL;
            file_put_contents($grubDefaultPath, $content);
            $cmd = new \OMV\System\Process('update-grub');
            $cmd->execute($output);
        } elseif (!preg_match('/^GRUB_DEFAULT\s*=\s*saved\s*$/m', $content)) {
            $new = preg_replace('/^GRUB_DEFAULT\s*=.*$/m', 'GRUB_DEFAULT=saved', $content);
            if ($new !== null && $new !== $content) {
                file_put_contents($grubDefaultPath, $new);
               $cmd = new \OMV\System\Process('update-grub');
               $cmd->execute($output);
            }
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

        $kernels = [];
        foreach ($entries as $e) {
            $entryId = $e['entry_id'];
            $name = $e['title'];
            $kver = $e['kernel'];

            // Determine default:
            // Prefer explicit saved_entry value (when it is an entry_id != 'saved'),
            // otherwise fall back to matching running kernel version.
            $isDefault = false;
            if (is_string($savedEntry) && $savedEntry !== '' && $savedEntry !== 'saved') {
                $isDefault = ($savedEntry === $entryId || $savedEntry === $name);
            } elseif (!empty($currentKernel) && !empty($kver)) {
                $isDefault = str_starts_with($currentKernel, $kver);
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

            $kernels[] = [
                'name' => $name,
                'key' => $kver,
                'entry_id' => $entryId,
                'default' => $isDefault,
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
        $cmdArgs = [];
        $cmdArgs[] = 'list';
        $cmdArgs[] = sprintf('| awk -F \'=\' \'$1 == "%s" { print $2 }\'', $key);
        $cmd = new \OMV\System\Process('grub-editenv', $cmdArgs);
        $cmd->execute($output);
        return isset($output[0]) ? trim($output[0]) : null;
    }

    public function deleteKernel($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // Check if the package database is locked.
        \OMV\System\Apt::assertNotLocked();
        // Create the background process.
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
          use ($params) {
            $kernel = explode(' ', $params['name']);
            // Upgrade the given packages.
            $cmdArgs = [];
            $cmdArgs[] = "dpkg -l |";
            $cmdArgs[] = sprintf("grep %s", $kernel[4]);
            $cmdArgs[] = "| cut -d' ' -f3";
            $cmdArgs[] = "| grep -E 'proxmox-kernel|pve-kernel|linux-image'";
            $cmdArgs[] = "| xargs";
            $cmdArgs[] = "apt-get";
            $cmdArgs[] = "--yes";
            $cmdArgs[] = "purge";
            $cmd = new \OMV\System\Process($cmdArgs);
            $cmd->setRedirect2to1();
            $cmd->setEnv("DEBIAN_FRONTEND", "noninteractive");
            if (0 !== ($exitStatus = $this->exec($cmd->getCommandLine(), $output, $bgOutputFilename))) {
                throw new \OMV\ExecException($cmd->getCommandLine(), $output, $exitStatus);
            }
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
}
