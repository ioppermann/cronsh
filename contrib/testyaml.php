#!/opt/local/bin/php56
<?php
	$input = stream_get_contents(STDIN);
	$line = explode("\n", $input);

	$i = 0;
	foreach($line as $l) {
		printf("%4d: %s\n", $i, $l);
		$i++;
	}

	$data = yaml_parse($input);

	if($data !== FALSE) {
		print_r($data);
		echo "\n";
	}
	else {
		echo "invalid YAML document\n";
	}
?>
