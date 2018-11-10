/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import * as React from 'react'
import { bindActionCreators, Dispatch } from 'redux'
import { connect } from 'react-redux'

// Feature-specific components
import {
  Background,
  Page,
  Panel,
  BackgroundContainer,
  SlideContent
} from 'brave-ui/features/welcome'

// Component groups
import WelcomeBox from './screens/welcomeBox'
import ImportBox from './screens/importBox'
import RewardsBox from './screens/rewardsBox'
import SearchBox from './screens/searchBox'
import ShieldsBox from './screens/shieldsBox'
import ThemeBox from './screens/themeBox'
import FooterBox from './screens/footerBox'

// Utils
import * as welcomeActions from '../actions/welcome_actions'

// Assets
const background = require('../../img/welcome/welcome_bg.svg')
require('../../fonts/muli.css')
require('../../fonts/poppins.css')
require('emptykit.css')

interface Props {
  welcomeData: Welcome.State
  actions: any
}

export interface State {
  currentScreen: number
}

export class WelcomePage extends React.Component<Props, State> {
  constructor (props: Props) {
    super(props)
    this.state = {
      currentScreen: 1
    }
  }

  get totalScreensSize () {
    return 6
  }

  get currentScreen () {
    return this.state.currentScreen
  }

  get backgroundPosition () {
    const lookUp = { 1: '100%', 2: '200%', 3: '300%', 4: '400%', 5: '500%', 6: '600%' }
    return lookUp[this.currentScreen]
  }

  onClickLetsGo = () => {
    this.setState({ currentScreen: this.state.currentScreen + 1 })
  }

  onClickImport = () => {
    // clicking this button executes functionality and then auto proceed to next screen
    this.props.actions.importNowRequested()
    this.setState({ currentScreen: this.state.currentScreen + 1 })
  }

  onClickConfirmDefaultSearchEngine = () => {
    this.props.actions.goToTabRequested('brave://settings/search', '_blank')
  }

  onClickChooseYourTheme = () => {
    this.props.actions.goToTabRequested('brave://settings/appearance', '_blank')
  }

  onClickRewardsGetStarted = () => {
    this.props.actions.goToTabRequested('brave://rewards', '_blank')
  }

  onClickSlideBullet = (nextScreen: number) => {
    this.setState({ currentScreen: nextScreen })
  }

  onClickNext = () => {
    this.setState({ currentScreen: this.state.currentScreen + 1 })
  }

  onClickDone = () => {
    this.props.actions.goToTabRequested('brave://newtab', '_self')
  }

  onClickSkip = () => {
    this.props.actions.goToTabRequested('brave://newtab', '_self')
  }

  render () {
    return (
      <>
        <BackgroundContainer>
          <Background background={{ image: background, position: `-${this.currentScreen}0%` }} />
        </BackgroundContainer>
        <Page id='welcomePage'>
          <Panel>
            <SlideContent>
              <WelcomeBox index={1} currentScreen={this.currentScreen} onClick={this.onClickLetsGo} />
              <ImportBox index={2} currentScreen={this.currentScreen} onClick={this.onClickImport} />
              <SearchBox index={3} currentScreen={this.currentScreen} onClick={this.onClickConfirmDefaultSearchEngine} />
              <ThemeBox index={4} currentScreen={this.currentScreen} onClick={this.onClickChooseYourTheme} />
              <ShieldsBox index={5} currentScreen={this.currentScreen} />
              <RewardsBox index={6} currentScreen={this.currentScreen} onClick={this.onClickRewardsGetStarted} />
            </SlideContent>
            <FooterBox
              totalScreensSize={this.totalScreensSize}
              currentScreen={this.currentScreen}
              onClickSkip={this.onClickSkip}
              onClickSlideBullet={this.onClickSlideBullet}
              onClickNext={this.onClickNext}
              onClickDone={this.onClickDone}
            />
          </Panel>
        </Page>
      </>
    )
  }
}

export const mapStateToProps = (state: Welcome.ApplicationState) => ({
  welcomeData: state.welcomeData
})

export const mapDispatchToProps = (dispatch: Dispatch) => ({
  actions: bindActionCreators(welcomeActions, dispatch)
})

export default connect(
  mapStateToProps,
  mapDispatchToProps
)(WelcomePage)
