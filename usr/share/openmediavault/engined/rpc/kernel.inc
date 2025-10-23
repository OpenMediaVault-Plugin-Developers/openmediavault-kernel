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
        $this->registerMethod('bootKernelOnce');
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
        // Set boot kernel
        $grub = '/etc/default/grub';
        if (file_exists($grub)) {
            $cmdArgs = sprintf('-p -i -e "s/GRUB_DEFAULT=.*/GRUB_DEFAULT=%s/g" %s', $params['key'], $grub);
            $output = [];
            $cmd = new \OMV\System\Process('perl', $cmdArgs);
            $cmd->execute($output);
            $output = [];
            $cmd = new \OMV\System\Process('grub-set-default', $params['key']);
            $cmd->execute($output);
            $output = [];
            $cmd = new \OMV\System\Process('update-grub');
            $cmd->execute($output);
        }
    }

    public function bootKernelOnce($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // Create the background process.
        return $this->execBgProc(function($bgStatusFilename, $bgOutputFilename)
            use ($params) {
                $cmdParams = [];
                $cmdParams[] = '/usr/sbin/omv-kernel-reboot-once';
                $cmdParams[] = sprintf("'%s'", $params['name']);
                $cmd = new \OMV\System\Process($cmdParams);
                $cmdLine = $cmd->getCommandLine();
                if (0 !== $this->exec($cmdLine, $output, $bgOutputFilename))
                    throw new \OMV\ExecException($cmdLine, $output);
                return $output;
            }
        );
    }

    private function getCurrentKernel() {
        $grubCfg = '/boot/grub/grub.cfg';
        $currentKernel = php_uname('r');
        if (!is_readable($grubCfg)) {
            return null;
        }
        $lines = file($grubCfg);
        $menuIndex = -1;
        $submenuIndex = -1;
        $inSubmenu = false;
        foreach ($lines as $line) {
            $trimmed = trim($line);
            if (preg_match('/^submenu\s+["\'].*["\']\s*{/', $trimmed)) {
                $inSubmenu = true;
                $submenuIndex = -1;
                $menuIndex++;
            } elseif (preg_match('/^menuentry\s+["\'].*["\']\s*{/', $trimmed)) {
                if ($inSubmenu) {
                    $submenuIndex++;
                } else {
                    $menuIndex++;
                    $submenuIndex = -1;
                }
            } elseif (preg_match('/^\s*linux\s+.*vmlinuz-(\S+)/', $trimmed, $matches)) {
                $entryKernel = $matches[1];
                if (str_starts_with($currentKernel, $entryKernel)) {
                    return $submenuIndex >= 0 ? "{$menuIndex}>{$submenuIndex}" : (string)$menuIndex;
                }
            } elseif (strpos($trimmed, '}') !== false && $inSubmenu) {
                $inSubmenu = false;
            }
        }
        return null;
    }

    public function getKernelList($params, $context)
    {
        // Validate the RPC caller context
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // get current kernel
        $defaultKernel = $this->getGrubEnvValue('saved_entry');
        if ($defaultKernel === 'saved') {
            $curKernel = $this->getCurrentKernel();
            $defaultKernel = is_numeric($curKernel) ? (int)$curKernel : -1;
        } else {
            $defaultKernel = is_numeric($defaultKernel) ? (int)$defaultKernel : -1;
        }
        // get boot once
        $nextEntry = $this->getGrubEnvValue('next_entry');
        $nextEntry = is_numeric($nextEntry) ? (int)$nextEntry : -1;
        // get list
        $kernels = [];
        $grubCfg = '/boot/grub/grub.cfg';
        if (is_readable($grubCfg)) {
            $output = [];
            $awkArgs = '-F "\'|\"" \'$1 == "menuentry " { print $2 }\'';
            $cmd = new \OMV\System\Process('awk', $awkArgs, $grubCfg);
            $cmd->execute($output);
            foreach ($output as $index => $name) {
                $name = trim($name);
                if (empty($name)) continue;
                $remove = false;
                $default = ($index === $defaultKernel);
                $nextk = ($index === $nextEntry);
                if (!$default) {
                    $hasDebian = strpos($name, 'Debian GNU/Linux, with Linux') !== false;
                    $hasProxmox = strpos($name, 'Proxmox Virtual Environment GNU/Linux, with Linux') !== false;
                    $isRecovery = strpos($name, 'recovery mode') !== false;
                    if (!$isRecovery && ($hasDebian || $hasProxmox)) {
                        $remove = true;
                    }
                }
                $kernels[] = [
                    'key' => $index,
                    'name' => $name,
                    'default' => $default,
                    'nextk' => $nextk,
                    'remove' => $remove
                ];
            }
        }
        return $this->applyFilter($kernels, $params['start'], $params['limit'],
            $params['sortfield'], $params['sortdir']
        );
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
