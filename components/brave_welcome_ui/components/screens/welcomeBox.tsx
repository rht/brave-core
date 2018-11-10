/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import * as React from 'react'

// Feature-specific components
import { Content, Title, BraveImage, Paragraph } from 'brave-ui/features/welcome/'

// Shared components
import { Button } from 'brave-ui'
import { ArrowRightIcon } from 'brave-ui/components/icons'

// Utils
import { getLocale } from '../../../common/locale'

// Images
const braveLogo = require('../../../img/welcome/lion_logo.svg')

interface Props {
  index: number
  currentScreen: number
  onClick: () => void
}

export default class ThemingBox extends React.PureComponent<Props, {}> {
  render () {
    const { index, currentScreen, onClick } = this.props
    return (
      <Content
        zIndex={index}
        active={currentScreen === index}
        screenPosition={'1' + (index + 1) + '0%'}
        isPrevious={index <= currentScreen}
      >
        <BraveImage src={braveLogo} />
        <Title>{getLocale('welcome')}</Title>
        <Paragraph>{getLocale('whatIsBrave')}</Paragraph>
        <Button
          level='primary'
          type='accent'
          size='large'
          text={getLocale('letsGo')}
          onClick={onClick}
          icon={{ position: 'after', image: <ArrowRightIcon /> }}
        />
      </Content>
    )
  }
}
