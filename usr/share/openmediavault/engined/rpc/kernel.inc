<?php
/**
 * @license   http://www.gnu.org/licenses/gpl.html GPL Version 3
 * @author    OpenMediaVault Plugin Developers <plugins@omv-extras.org>
 * @copyright Copyright (c) 2021-2023 OpenMediaVault Plugin Developers
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
        $this->registerMethod('doHold');
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

    public function getKernelList($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // Get boot kernel
        $grub = '/etc/default/grub';
        if (file_exists($grub)) {
            $output = [];
            $fnd = sprintf('grep -v "#" %s | awk -F "=" "/GRUB_DEFAULT/ { print \$2 }"', $grub);
            $cmd = new \OMV\System\Process($fnd);
            $cmd->execute($output);
            $defaultKernel = intval($output[0]);
        } else {
            $defaultKernel = -1;
        }
        // Get configuration data.
        $kernels = [];
        $grub = '/boot/grub/grub.cfg';
        if (file_exists($grub)) {
            $output = [];
            $awkArgs = '-F "\'|\"" \'$1 == "menuentry " { print $2 }\'';
            $cmd = new \OMV\System\Process('awk', $awkArgs, $grub);
            $cmd->execute($output);
            foreach ($output as $kernelk => $kernelv) {
                $remove = false;
                if (empty($kernelv))
                    continue;
                if ($kernelk == $defaultKernel) {
                    $default = true;
                } else {
                    $default = false;
                    $pos1 = strpos($kernelv, 'Debian GNU/Linux, with Linux');
                    $pos2 = strpos($kernelv, 'recovery mode');
                    if (false === $pos1 || false !== $pos2) {
                        $remove = false;
                    } else {
                        $remove = true;
                    }
                }
                $kernels[] = [
                    'key'  => $kernelk,
                    'name' => $kernelv,
                    'default' => $default,
                    'remove' => $remove
                ];
            }
        }
        return $this->applyFilter($kernels, $params['start'], $params['limit'],
            $params['sortfield'], $params['sortdir']
        );
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
                $cmdBin = 'omv-grubiso';
                $cmdParams = $params['command'];
                $cmd = new \OMV\System\Process($cmdBin, $cmdParams);
                $cmdLine = $cmd->getCommandLine();
                if (0 !== $this->exec($cmdLine, $output, $bgOutputFilename))
                    throw new \OMV\ExecException($cmdLine, $output);
                return $output;
            }
        );
    }

    public function doHold($params, $context)
    {
        // Validate the RPC caller context.
        $this->validateMethodContext($context, ['role' => OMV_ROLE_ADMINISTRATOR]);
        // Validate the parameters of the RPC service method.
        $this->validateMethodParams($params, 'rpc.kernel.dohold');
        // Execute command
        $arch = $this->getArch([], $context);
        $packages = sprintf('linux-image-%s linux-headers-%s', $arch['arch'], $arch['arch']);
        $cmd = new \OMV\System\Process('apt-mark', $params['command'], $packages);
        $cmd->execute($output);
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
